/**
 * @file ml_derp.c
 * @brief Unified DERP I/O Task + Connection Management
 *
 * Single task handles BOTH reading and writing to DERP TLS connection.
 * This eliminates the need for a TLS mutex since only one task touches
 * the SSL context. Matches v1's single-threaded DERP model.
 *
 * Architecture:
 * - Poll for incoming DERP frames (TLS read) every iteration
 * - Drain TX queue between reads (TLS write)
 * - No mutex needed — single task owns the SSL context exclusively
 *
 * Backpressure strategy (from tailscaled):
 * When queue is full, dequeue oldest packet and retry up to 3 times.
 * If still full, drop the new packet.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (runDerpWriter)
 *            tailscale/derp/derphttp/derphttp_client.go
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "nacl_box.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "ml_derp";

/* Timeout for DERP connection handshake operations. Bumped 10s -> 25s:
 * on a lossy/high-latency captive network (healthspot) the TLS handshake
 * itself was observed taking ~7.5s and occasionally exceeding 10s under
 * retransmits, tripping "TLS handshake failed: timed out" -> full-retry
 * thrash. 25s lets a slow-but-valid handshake complete. Steady-state
 * frame polling still uses a 200ms read timeout (set after connect). */
#define DERP_CONNECT_TIMEOUT_MS  25000

/* ============================================================================
 * Custom BIO callbacks for TLS I/O
 *
 * These wrap the socket layer (ml_read_sock/ml_write_sock) so both the
 * lwIP and AT socket backends work transparently. recv is a plain blocking
 * f_recv bounded by SO_RCVTIMEO on the socket (no f_recv_timeout /
 * mbedtls_ssl_conf_read_timeout) — combining recv_timeout with a TLS 1.3
 * handshake fails with MBEDTLS_ERR_SSL_BAD_INPUT_DATA on some DERP relays.
 * ========================================================================== */

/**
 * Custom blocking recv for mbedtls BIO (f_recv signature, no timeout arg).
 * SO_RCVTIMEO on the socket bounds the wait (set at both the TCP-connect
 * phase and the post-handshake data phase); a timeout surfaces as
 * EAGAIN/EWOULDBLOCK, mapped to MBEDTLS_ERR_SSL_WANT_READ. Using a plain
 * f_recv (rather than f_recv_timeout + mbedtls_ssl_conf_read_timeout) avoids
 * a TLS 1.3 MBEDTLS_ERR_SSL_BAD_INPUT_DATA failure on some DERP relays —
 * matches the scheme ml_coord_tls.c-style control-plane TLS already uses.
 */
static int ml_derp_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return ret;
}

/**
 * Custom send for mbedtls BIO.
 */
static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

/* DERP frame types */
#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* ============================================================================
 * TLS Read/Write Helpers
 * ========================================================================== */

/**
 * Read exactly `len` bytes via TLS with timeout and WANT_READ retry.
 * Returns number of bytes read on success, -1 on error, -2 on timeout.
 */
static int derp_tls_read_all(microlink_t *ml, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = mbedtls_ssl_read(&ml->derp.ssl, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}

/**
 * Read a DERP frame header (5 bytes: type + 4-byte BE length) with timeout.
 */
static esp_err_t derp_recv_frame_header(microlink_t *ml, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(ml, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}

/**
 * Write exactly `len` bytes via TLS with WANT_WRITE retry.
 * Returns bytes written on success, -1 on error.
 * No mutex needed — called only from the DERP I/O task.
 */
static int derp_tls_write_all(microlink_t *ml, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    /* 500ms was too tight: the caller treats any -1 here as fatal and forces
     * a full DERP reconnect, so a single congested write used to trigger a
     * reconnect storm. WANT_READ/WANT_WRITE/TIMEOUT are transient backpressure,
     * not real errors -- give them 3s before giving up. Genuine mbedtls errors
     * (the other branch below) still bail out on the first try. */
    const int max_retries = 300;  /* 300 * 10ms = 3s max */

    while (written < len) {
        int ret = mbedtls_ssl_write(&ml->derp.ssl, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        written += ret;
        retries = 0;
    }
    return (int)written;
}

/* Write a complete DERP frame via TLS */
static int derp_write_frame(microlink_t *ml, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    /* 5-byte header: 1 type + 4 length (big-endian) */
    uint8_t header[5];
    header[0] = type;
    header[1] = (len >> 24) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 8) & 0xFF;
    header[4] = len & 0xFF;

    if (derp_tls_write_all(ml, header, 5) < 0) return -1;

    if (len > 0 && payload) {
        if (derp_tls_write_all(ml, payload, len) < 0) return -1;
    }
    return 0;
}

/* Send a packet to a peer via DERP */
static int derp_send_packet(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    /* SendPacket frame built as one buffer (5-byte header + 32-byte dest
     * key + payload) instead of going through derp_write_frame's separate
     * header write + payload write -- one malloc, one TLS write, halving
     * the syscall/TLS-record count on this hot path. */
    size_t payload_len = 32 + len;
    size_t total_len = 5 + payload_len;
    uint8_t *buf = ml_psram_malloc(total_len);
    if (!buf) return -1;

    buf[0] = DERP_FRAME_SEND_PACKET;
    buf[1] = (payload_len >> 24) & 0xFF;
    buf[2] = (payload_len >> 16) & 0xFF;
    buf[3] = (payload_len >> 8) & 0xFF;
    buf[4] = payload_len & 0xFF;
    memcpy(buf + 5, dest_key, 32);
    memcpy(buf + 5 + 32, data, len);

    int wrote = derp_tls_write_all(ml, buf, total_len);
    int ret = (wrote < 0) ? -1 : 0;
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len);
    }
    free(buf);
    return ret;
}

/* ============================================================================
 * DERP Frame Reading and Dispatch (runs on DERP I/O task)
 * ========================================================================== */

/* Packet classification */
typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    }
}

/* Dispatch a received DERP frame */
static void dispatch_derp_frame(microlink_t *ml, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            ESP_LOGI(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF);
            route_derp_packet(ml, payload, payload_len, src_key);
            return;  /* payload ownership transferred */
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        /* Echo ping data back as PONG directly (single-threaded, safe to write) */
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(ml, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        if (payload && payload_len >= 32) {
            ESP_LOGI(TAG, "DERP PeerGone: %02x%02x%02x%02x (len=%d)",
                     payload[0], payload[1], payload[2], payload[3],
                     (int)payload_len);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}

/**
 * Try to read one DERP frame.
 * SO_RCVTIMEO on the socket bounds each read so ssl_read never blocks
 * indefinitely (timeout surfaces as WANT_READ from the blocking-recv BIO).
 * Returns: 1 = frame read and dispatched, 0 = timeout (no data), <0 = error
 */
static int poll_derp_read(microlink_t *ml) {
    if (!ml->derp.connected || ml->derp.sockfd < 0) return -1;

    /* Read 5-byte frame header.
     * SO_RCVTIMEO=100ms ensures read() returns within 100ms if no data. */
    uint8_t header[5];
    int n = mbedtls_ssl_read(&ml->derp.ssl, header, 5);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
        return 0;  /* No data available / timeout */
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "DERP header read returned %d (0x%04x)", n, n < 0 ? -n : 0);
        return n;
    }
    if (n < 5) {
        ESP_LOGW(TAG, "DERP partial header: got %d of 5 bytes", n);
        return -1;
    }

    uint8_t frame_type = header[0];
    uint32_t len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (len == 0) {
        dispatch_derp_frame(ml, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    /* Read frame payload - we already got the header so payload should follow.
     * Use longer timeout (2s) since we KNOW data is coming. */
    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        /* Safety timeout: 5 seconds for payload */
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = mbedtls_ssl_read(&ml->derp.ssl, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return n;
        }
        total_read += n;
    }

    /* For RecvPacket (0x05): first 32 bytes are sender's public key */
    if (frame_type == DERP_FRAME_RECV_PACKET && len > 32) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, frame_type, src_key, payload, payload_len);
    return 1;
}

/* ============================================================================
 * DERP TX Queue Processing
 * ========================================================================== */

/* Queue a packet for DERP TX with backpressure */
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
    };
    memcpy(item.dest_pubkey, dest_key, 32);

    /* WG handshake packets (type 1=init, 2=response) get priority — front of queue.
     * This ensures handshake responses aren't delayed behind DISCO pings. */
    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    /* Try to send to queue */
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        return ESP_OK;
    }

    /* Queue full - backpressure: drop oldest, retry up to 3 times */
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  /* Drop oldest */
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            return ESP_OK;
        }
    }

    /* Still full after 3 attempts, drop new packet */
    free(pkt_data);
    return ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * Unified DERP I/O Task
 * ========================================================================== */

void ml_derp_tx_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "DERP I/O task started (Core %d)", xPortGetCoreID());

    uint32_t frames_rx = 0;
    uint32_t frames_tx = 0;
    uint64_t last_status_ms = 0;
    uint32_t loop_count = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t connected_since_ms = 0;
    bool verbose_phase = false;  /* verbose logging for first 15s after connect */

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        loop_count++;
        uint64_t loop_start = ml_get_time_ms();

        /* Unconditional heartbeat - proves task is alive */
        if (loop_start - last_heartbeat_ms > 5000) {
            ESP_LOGW(TAG, "HEARTBEAT: loop=%lu conn=%d rx=%lu tx=%lu stack_free=%lu",
                     (unsigned long)loop_count, ml->derp.connected,
                     (unsigned long)frames_rx, (unsigned long)frames_tx,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = loop_start;
        }

        /* ---- Periodic status logging (always, even when disconnected) ---- */
        {
            uint64_t now_ms = loop_start;
            if (now_ms - last_status_ms > 10000) {
                ESP_LOGI(TAG, "DERP status: connected=%d fd=%d rx=%lu tx=%lu loops=%lu",
                         ml->derp.connected, ml->derp.sockfd,
                         (unsigned long)frames_rx, (unsigned long)frames_tx,
                         (unsigned long)loop_count);
                last_status_ms = now_ms;
            }
        }

        /* ---- Handle DERP connect request from coord task ---- */
        {
            EventBits_t bits = xEventGroupGetBits(ml->events);
            if ((bits & ML_EVT_DERP_CONNECT_REQ) && !ml->derp.connected) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                /* Retry up to 3 times with 2s backoff. Each attempt can cost
                 * a DNS timeout plus a 10s TLS handshake, so re-check for
                 * shutdown between them — three blind retries is how this
                 * task used to outlive microlink_stop()'s join window. */
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP connect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        ESP_LOGI(TAG, "DERP connect requested, connecting from I/O task");
                    }
                    if (ml_shutdown_pending(ml)) {
                        ESP_LOGI(TAG, "Shutdown during DERP connect; abandoning retries");
                        break;
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP connect attempt %d failed", attempt + 1);
                }
            }
            if (bits & ML_EVT_DERP_RECONNECT) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_RECONNECT);
                ESP_LOGW(TAG, "DERP reconnect requested (was %s)",
                         ml->derp.connected ? "connected" : "disconnected");
                ml_derp_disconnect(ml);
                verbose_phase = false;
                /* Auto-reconnect after disconnect */
                vTaskDelay(pdMS_TO_TICKS(1000));
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP reconnect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ml_shutdown_pending(ml)) {
                        ESP_LOGI(TAG, "Shutdown during DERP reconnect; abandoning retries");
                        break;
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP reconnect attempt %d failed", attempt + 1);
                }
            }
        }

        /* Disable verbose logging after 15s */
        if (verbose_phase && ml_get_time_ms() - connected_since_ms > 15000) {
            verbose_phase = false;
        }

        if (!ml->derp.connected) {
            /* Not connected - just wait and check again */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool did_work = false;

        /* ---- Phase 1: Drain TX queue FIRST (prioritize outgoing) ---- */
        {
            for (int tx_count = 0; tx_count < 32; tx_count++) {
                ml_derp_tx_item_t item;
                if (xQueueReceive(ml->derp_tx_queue, &item, 0) != pdTRUE) {
                    break;
                }
                did_work = true;
                if (!ml->derp.connected) {
                    free(item.data);
                    continue;
                }
                int ret;
                if (item.frame_type == DERP_FRAME_SEND_PACKET) {
                    ESP_LOGI(TAG, "DERP TX: SendPacket %d bytes, dest=%02x%02x%02x%02x, hdr=%02x",
                             (int)item.len, item.dest_pubkey[0], item.dest_pubkey[1],
                             item.dest_pubkey[2], item.dest_pubkey[3],
                             item.data[0]);
                    ret = derp_send_packet(ml, item.dest_pubkey, item.data, item.len);
                } else {
                    ret = derp_write_frame(ml, item.frame_type, item.data, item.len);
                }
                if (ret < 0) {
                    ESP_LOGW(TAG, "DERP write failed");
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                } else {
                    frames_tx++;
                }
                free(item.data);
            }
        }

        if (!ml->derp.connected) continue;

        /* ---- Phase 2: Poll for incoming DERP frames ---- */
        {
            int burst;
            for (burst = 0; burst < 32; burst++) {
                int ret = poll_derp_read(ml);
                if (ret > 0) {
                    frames_rx++;
                    did_work = true;
                    ml->derp.last_recv_ms = ml_get_time_ms();
                } else if (ret == 0) {
                    break;  /* No more data / timeout */
                } else {
                    ESP_LOGW(TAG, "DERP read error: %d", ret);
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    break;
                }
            }
        }

        /* pdMS_TO_TICKS(1) rounds down to 0 ticks at this project's 100Hz
         * tick rate, so the old unconditional delay never actually paced
         * an idle loop -- it spun at full speed polling a non-blocking
         * socket. Only yield the CPU when there was nothing to do; when
         * genuinely idle, back off for real. */
        if (did_work) {
            taskYIELD();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* Release the TLS state, the socket and anything still queued for TX.
     * Nobody else can: microlink_stop() deliberately leaves derp.sockfd to
     * this task, and on the orphan path the context outlives the stop. */
    ml_derp_disconnect(ml);

    ESP_LOGI(TAG, "DERP I/O task exiting");
    ml_task_exit(ml, ML_TASK_BIT_DERP_TX);
}

/* ============================================================================
 * DERP Connection Management (called from coord task)
 * ========================================================================== */

/* Free the mbedtls state initialized in ml_derp_connect's TLS phase and close
 * the underlying socket. Used by both ml_derp_disconnect (graceful teardown,
 * after close_notify) and the fail_tls cleanup path in ml_derp_connect
 * (handshake-failure unwind), so the two call sites can't drift apart.
 * Adapted from cplewes/microlink@b25b1eee: our ml_derp_conn_t has no
 * entropy/ctr_drbg fields (RNG is PSA-owned post mbedTLS 4.x migration, see
 * ESP_IDF_6X_COMPAT.md), so this frees only ssl/ssl_conf. */
static void derp_free_tls_state(microlink_t *ml) {
    mbedtls_ssl_free(&ml->derp.ssl);
    mbedtls_ssl_config_free(&ml->derp.ssl_conf);
    if (ml->derp.sockfd >= 0) {
        ml_close_sock(ml->derp.sockfd);
        ml->derp.sockfd = -1;
    }
}

esp_err_t ml_derp_connect(microlink_t *ml) {
    /* Don't start a fresh DNS + TCP + TLS sequence the caller is about to
     * throw away — every phase below can block for seconds. */
    if (ml_shutdown_pending(ml)) {
        ESP_LOGI(TAG, "Shutdown requested; not connecting to DERP");
        return ESP_FAIL;
    }

    /* Determine DERP host/port from DERPMap with node failover.
     * Always start from node 0 (the first/preferred node in the DERPMap).
     * Only rotate to a different node after a SUCCESSFUL connection drops,
     * NOT on connection failure (to avoid bouncing between nodes). */
    const char *derp_host = ML_DERP_HOST;
    int derp_port = ML_DERP_PORT;
    /* Track the region actually used, for logging (differs from HomeDERP on fallback) */
    uint16_t derp_region_used = ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION;
    bool node_selected = false;

    if (ml->derp_region_count > 0 && ml->derp_home_region > 0) {
        for (int i = 0; i < ml->derp_region_count; i++) {
            if (ml->derp_regions[i].region_id == ml->derp_home_region) {
                /* Always use the first non-stun-only node (preferred node).
                 * This ensures we connect to the same node as most peers. */
                for (int attempt = 0; attempt < ml->derp_regions[i].node_count; attempt++) {
                    if (!ml->derp_regions[i].nodes[attempt].stun_only &&
                        ml->derp_regions[i].nodes[attempt].hostname[0]) {
                        derp_host = ml->derp_regions[i].nodes[attempt].hostname;
                        if (ml->derp_regions[i].nodes[attempt].derp_port > 0) {
                            derp_port = ml->derp_regions[i].nodes[attempt].derp_port;
                        }
                        node_selected = true;
                        break;
                    }
                }
                break;
            }
        }
    }

    /* Fallback for when HomeDERP is not present in the DERPMap (e.g. a
     * control plane that advertises its own region IDs outside Tailscale's
     * official numbering). Falling back to the default ML_DERP_HOST in that
     * case is a dead end — auth/TLS won't succeed against a mismatched
     * region — so pick the first usable node (not avoid, not stun-only, has
     * a hostname) from the DERPMap actually handed to us instead. In the
     * normal case HomeDERP is in the DERPMap so node_selected is already
     * true and this loop never runs; behavior is unchanged. */
    if (!node_selected && ml->derp_region_count > 0) {
        for (int i = 0; i < ml->derp_region_count && !node_selected; i++) {
            ml_derp_region_t *r = &ml->derp_regions[i];
            if (r->avoid) continue;
            for (int j = 0; j < r->node_count; j++) {
                if (!r->nodes[j].stun_only && r->nodes[j].hostname[0]) {
                    derp_host = r->nodes[j].hostname;
                    derp_port = (r->nodes[j].derp_port > 0) ? r->nodes[j].derp_port
                                                            : ML_DERP_PORT;
                    derp_region_used = r->region_id;
                    node_selected = true;
                    ESP_LOGW(TAG, "Home DERP region %d not in DERPMap, falling back to region %d (%s)",
                             ml->derp_home_region, derp_region_used, derp_host);
                    break;
                }
            }
        }
    }

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %d)",
             derp_host, derp_port, derp_region_used);

    /* DNS resolve — accept IPv4 or IPv6 (carrier may be IPv6-only) */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP DNS: %lld ms", (t_derp_dns - t_derp_start) / 1000);

    /* TCP connect — use address family from DNS result */
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    /* Set connect timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TCP connect: %lld ms", (t_derp_tcp - t_derp_dns) / 1000);

    /* TLS setup. CRITICAL: free any prior SSL state before re-init.
     * mbedtls_ssl_init() zeroes the struct without freeing internal heap
     * allocations; if a previous ml_derp_connect() got partway through the
     * setup calls below before failing (e.g. mid-retry on a captive-portal
     * network with blocked DNS), those buffers are orphaned and this re-init
     * loses the only pointers to them. Freeing first avoids both the leak
     * and a dangling-pointer reuse in mbedTLS's internal state on the next
     * handshake. mbedtls_ssl_free/config_free are safe to call on an
     * already-zeroed or already-freed struct — they check internal sentinel
     * fields before deref'ing. No entropy/ctr_drbg free here: those fields
     * don't exist in our ml_derp_conn_t (RNG is PSA-owned post mbedTLS 4.x
     * migration, see ESP_IDF_6X_COMPAT.md) — same adaptation as
     * derp_free_tls_state() above.
     * Adapted from cplewes/microlink@7120dfa4. */
    mbedtls_ssl_free(&ml->derp.ssl);
    mbedtls_ssl_config_free(&ml->derp.ssl_conf);

    mbedtls_ssl_init(&ml->derp.ssl);
    mbedtls_ssl_config_init(&ml->derp.ssl_conf);

    /* Check every mbedTLS setup return and abort before mbedtls_ssl_handshake.
     * A failing mbedtls_ssl_setup (typically ALLOC_FAILED under internal-RAM
     * pressure) NULLs ssl->conf, so proceeding to handshake anyway returns
     * BAD_INPUT_DATA and the real cause (OOM) shows up as a misleading
     * "Bad input parameters" instead. */
    int cfg_ret;
    cfg_ret = mbedtls_ssl_config_defaults(&ml->derp.ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "ssl_config_defaults failed: -0x%04x", -cfg_ret);
        goto fail_tls;
    }
    mbedtls_ssl_conf_authmode(&ml->derp.ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    /* Pin the DERP connection to TLS 1.2. When the DERP relay uses a
     * Let's Encrypt ECDSA certificate, the ESP-IDF mbedTLS TLS 1.3 path
     * can't parse the certificate's signature-algorithm OID and fails even
     * under VERIFY_NONE (TLS 1.3 can't skip certificate processing). DERP
     * also accepts TLS 1.2, so pin to 1.2 to bypass the 1.3 cert path. */
    mbedtls_ssl_conf_max_tls_version(&ml->derp.ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);

    cfg_ret = mbedtls_ssl_setup(&ml->derp.ssl, &ml->derp.ssl_conf);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_setup failed: -0x%04x", -cfg_ret);
        goto fail_tls;
    }
    cfg_ret = mbedtls_ssl_set_hostname(&ml->derp.ssl, derp_host);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "ssl_set_hostname failed: -0x%04x", -cfg_ret);
        goto fail_tls;
    }
    /* Store socket fd BEFORE setting bio.
     * Use custom BIO callbacks that route through ml_read_sock/ml_write_sock,
     * which transparently support both lwIP and AT socket backends.
     * Timeout is handled via SO_RCVTIMEO, not mbedtls_ssl_conf_read_timeout. */
    ml->derp.sockfd = sock;
    mbedtls_ssl_set_bio(&ml->derp.ssl, &ml->derp.sockfd,
                         ml_derp_bio_send, ml_derp_bio_recv, NULL);

    /* Resume a prior TLS session if we saved one from a previous DERP
     * connection (saved_session survives derp_free_tls_state across a reco).
     * A resumed handshake skips ECDHE -> sub-second instead of ~7.5s. */
    if (ml->derp.have_saved_session) {
        int sret = mbedtls_ssl_set_session(&ml->derp.ssl, &ml->derp.saved_session);
        if (sret == 0) {
            ESP_LOGI(TAG, "DERP TLS: resuming saved session");
        } else {
            ESP_LOGW(TAG, "DERP TLS: set_session -0x%04x, doing full handshake", -sret);
        }
    }

    /* TLS handshake - socket has SO_RCVTIMEO from connect phase. */
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ml->derp.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* This spin is where the task can sit for tens of seconds on a
             * captive network — each pass costs a full socket read timeout.
             * Bail out on shutdown so the teardown join doesn't have to wait
             * for the server to answer; fail_tls frees the mbedTLS state. */
            if (ml_shutdown_pending(ml)) {
                ESP_LOGW(TAG, "Shutdown during TLS handshake; aborting");
                goto fail_tls;
            }
            continue;
        }
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "TLS handshake failed: %s", err_buf);
        goto fail_tls;
    }

    int64_t t_derp_tls = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TLS handshake: %lld ms", (t_derp_tls - t_derp_tcp) / 1000);
    ESP_LOGI(TAG, "TLS connected to DERP");

    /* Save the negotiated session so the NEXT reconnect can resume it and
     * skip the expensive full handshake. get_session deep-copies the ticket,
     * so free any prior copy first. Best-effort: on failure we just do a full
     * handshake next time. */
    if (ml->derp.have_saved_session) {
        mbedtls_ssl_session_free(&ml->derp.saved_session);
        ml->derp.have_saved_session = false;
    }
    mbedtls_ssl_session_init(&ml->derp.saved_session);
    if (mbedtls_ssl_get_session(&ml->derp.ssl, &ml->derp.saved_session) == 0) {
        ml->derp.have_saved_session = true;
    } else {
        mbedtls_ssl_session_free(&ml->derp.saved_session);
    }

    /* HTTP Upgrade: GET /derp with Upgrade: DERP header */
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = mbedtls_ssl_write(&ml->derp.ssl, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send HTTP upgrade");
        goto fail_tls;
    }

    /* Read HTTP response byte-by-byte until \r\n\r\n to avoid over-reading
     * into the DERP binary frame stream (matching v1 approach) */
    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "HTTP upgrade response timeout");
                goto fail_tls;
            }

            ret = mbedtls_ssl_read(&ml->derp.ssl, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                goto fail_tls;
            }
            if (ret == 0) {
                ESP_LOGE(TAG, "Connection closed during HTTP upgrade");
                goto fail_tls;
            }
            resp_len++;

            /* Check for \r\n\r\n */
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strstr((char *)resp_buf, "101") == NULL) {
            ESP_LOGE(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            goto fail_tls;
        }
        ESP_LOGI(TAG, "HTTP 101 Switching Protocols received");
    }

    /* Match v1 exactly: O_NONBLOCK + short SO_RCVTIMEO + SO_SNDTIMEO.
     * O_NONBLOCK ensures read()/write() never block indefinitely.
     * SO_RCVTIMEO provides 100ms polling rhythm for reads.
     * SO_SNDTIMEO prevents writes from blocking too long. */
    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
        /* Nagle's algorithm would coalesce our own already-small DERP frames
         * with an up-to-500ms delay waiting for more data -- pure added
         * latency on a link the app layer already keeps busy. */
        int nodelay = 1;
        ml_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    /* ========================================================
     * DERP Handshake: ServerKey -> ClientInfo -> ServerInfo
     * ======================================================== */

    /* Step 1: Read ServerKey frame header using reliable read helper */
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(ml, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        goto fail_tls;
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGE(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        goto fail_tls;
    }

    /* Read and verify 8-byte magic */
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(ml, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read ServerKey magic");
        goto fail_tls;
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        goto fail_tls;
    }
    ESP_LOGI(TAG, "DERP magic verified");

    /* Read 32-byte server public key */
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(ml, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read server key");
        goto fail_tls;
    }

    ESP_LOGI(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    /* Skip remaining bytes if frame_len > 40 */
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(ml, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    /* Step 2: Send ClientInfo frame (type 0x02)
     * Payload: [our_nodekey(32)][nonce(24)][nacl_box(JSON)] */
    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        /* Generate random nonce */
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        /* Encrypt JSON with NaCl box: our WG private key -> DERP server public key */
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            goto fail_tls;
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       /* recipient: DERP server */
                     ml->wg_private_key     /* sender: our WG node key */
                     ) != 0) {
            ESP_LOGE(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            goto fail_tls;
        }

        /* Build ClientInfo frame payload: nodekey(32) + nonce(24) + ciphertext */
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            goto fail_tls;
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGI(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        /* Send ClientInfo frame */
        if (derp_write_frame(ml, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGE(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            goto fail_tls;
        }
        free(ci_payload);

        ESP_LOGI(TAG, "ClientInfo sent");
    }

    /* Step 3: Read ServerInfo frame (type 0x03) */
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(ml, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK && si_type == DERP_FRAME_SERVER_INFO && si_len > 0) {
            /* Read and discard ServerInfo payload */
            uint8_t *si_buf = malloc(si_len);
            if (si_buf) {
                derp_tls_read_all(ml, si_buf, si_len, DERP_CONNECT_TIMEOUT_MS);
                free(si_buf);
            }
            ESP_LOGI(TAG, "ServerInfo received (discarded)");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ServerInfo frame (continuing anyway)");
        }
    }

    /* Send NotePreferred (type 0x07): this is our preferred DERP */
    {
        uint8_t preferred = 0x01;
        derp_write_frame(ml, DERP_FRAME_NOTE_PREFERRED, &preferred, 1);
    }

    /* Switch socket to short timeout for data phase.
     * Long timeout was needed for TLS handshake, but polling must be fast.
     * conf_read_timeout is deliberately not called: recv uses the blocking
     * f_recv scheme (f_recv_timeout is NULL), so only SO_RCVTIMEO bounds
     * reads — same policy as the TLS 1.3 Bad Input Data workaround above. */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
        ml_setsockopt(ml->derp.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ml->derp.connected = true;
    ml->derp.last_recv_ms = ml_get_time_ms();
    xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %lld ms (DNS=%lld, TCP=%lld, TLS=%lld, proto=%lld)",
             (t_derp_done - t_derp_start) / 1000,
             (t_derp_dns - t_derp_start) / 1000,
             (t_derp_tcp - t_derp_dns) / 1000,
             (t_derp_tls - t_derp_tcp) / 1000,
             (t_derp_done - t_derp_tls) / 1000);
    ESP_LOGI(TAG, "DERP handshake complete, connected");
    return ESP_OK;

fail_tls:
    /* Reached on any error after mbedtls_ssl_init/mbedtls_ssl_config_init.
     * Without this teardown each failed handshake leaks the mbedtls state
     * (~3-8 KB internal heap) until even a fresh handshake can't allocate
     * buffers — microlink_rebind() reconnects DERP on every WiFi
     * reconnect, so this compounds fast under captive-portal/bad-network
     * conditions. Adapted from cplewes/microlink@38602ab0/@b25b1eee. */
    derp_free_tls_state(ml);
    return ESP_FAIL;
}

void ml_derp_disconnect(microlink_t *ml) {
    ml->derp.connected = false;
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);

    if (ml->derp.sockfd >= 0) {
        mbedtls_ssl_close_notify(&ml->derp.ssl);
        derp_free_tls_state(ml);
    }

    /* Drain TX queue */
    ml_derp_tx_item_t item;
    while (xQueueReceive(ml->derp_tx_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }

    ESP_LOGI(TAG, "DERP disconnected");
}
