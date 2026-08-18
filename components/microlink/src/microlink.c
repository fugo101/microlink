/**
 * @file microlink.c
 * @brief MicroLink v2 - Public API and Task Orchestration
 *
 * Creates all FreeRTOS tasks, queues, and event groups.
 * Provides the public API that the application calls.
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include "lwip/sockets.h"

#ifdef CONFIG_ML_ENABLE_CELLULAR
#include "ml_cellular.h"
#endif

static const char *TAG = "microlink";

/* NVS keys */
#define NVS_NAMESPACE       "microlink"
#define NVS_KEY_MACHINE_PRI "machine_pri"
#define NVS_KEY_MACHINE_PUB "machine_pub"
#define NVS_KEY_WG_PRI      "wg_private"
#define NVS_KEY_WG_PUB      "wg_public"
#define NVS_KEY_DISCO_PRI   "disco_pri"
#define NVS_KEY_DISCO_PUB   "disco_pub"

/* X25519 from x25519.h */
#include "x25519.h"

/* ============================================================================
 * Key Management (loaded once at init, read-only after)
 * ========================================================================== */

static void generate_keypair(uint8_t *private_key, uint8_t *public_key) {
    esp_fill_random(private_key, 32);
    private_key[0] &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    x25519_base(public_key, private_key, 1);
}

static esp_err_t load_or_generate_keys(microlink_t *ml) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%d), generating ephemeral keys", err);
        generate_keypair(ml->machine_private_key, ml->machine_public_key);
        generate_keypair(ml->wg_private_key, ml->wg_public_key);
        generate_keypair(ml->disco_private_key, ml->disco_public_key);
        return ESP_OK;
    }

    size_t key_len = 32;
    bool need_save = false;

    /* Machine key */
    if (nvs_get_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->machine_private_key, ml->machine_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new machine key");
    } else {
        key_len = 32;
        nvs_get_blob(nvs, NVS_KEY_MACHINE_PUB, ml->machine_public_key, &key_len);
    }

    /* WireGuard key */
    key_len = 32;
    if (nvs_get_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->wg_private_key, ml->wg_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new WireGuard key");
    } else {
        key_len = 32;
        nvs_get_blob(nvs, NVS_KEY_WG_PUB, ml->wg_public_key, &key_len);
    }

    /* DISCO key */
    key_len = 32;
    if (nvs_get_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->disco_private_key, ml->disco_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new DISCO key");
    } else {
        key_len = 32;
        nvs_get_blob(nvs, NVS_KEY_DISCO_PUB, ml->disco_public_key, &key_len);
    }

    if (need_save) {
        nvs_set_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_MACHINE_PUB, ml->machine_public_key, 32);
        nvs_set_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_WG_PUB, ml->wg_public_key, 32);
        nvs_set_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_DISCO_PUB, ml->disco_public_key, 32);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Keys saved to NVS");
    } else {
        ESP_LOGI(TAG, "Keys loaded from NVS");
    }

    nvs_close(nvs);
    return ESP_OK;
}

/* ============================================================================
 * cJSON PSRAM Hooks
 * ========================================================================== */

static void *cjson_psram_malloc(size_t size) {
    return ml_psram_malloc(size);
}

/* ============================================================================
 * Credential Check
 * ========================================================================== */

bool microlink_has_stored_credentials(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t key[32];
    size_t len = sizeof(key);
    bool found = (nvs_get_blob(nvs, NVS_KEY_MACHINE_PRI, key, &len) == ESP_OK);
    nvs_close(nvs);
    return found;
}

/* ============================================================================
 * Factory Reset
 * ========================================================================== */

esp_err_t microlink_factory_reset(void) {
    esp_err_t err;

    /* Erase key namespace */
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Factory reset: keys erased");
    }

    /* Erase cached peers */
    ml_peer_nvs_init();
    ml_peer_nvs_clear();
    ml_peer_nvs_deinit();

    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}

/* ============================================================================
 * Teardown contract
 *
 * The four worker tasks (net_io, derp_tx, coord, wg_mgr) hold a raw pointer
 * to the context for their whole life, so the context may only be freed once
 * every one of them has *provably* stopped running. Proof is the
 * ml->tasks_running bitmask: microlink_start() sets a task's bit before
 * creating it, and the task clears its own bit from ml_task_exit() as its
 * final act, after which it touches nothing owned by the context.
 *
 * This replaces the previous "set the shutdown bit, sleep 3 s, assume they
 * are gone" teardown. A worker blocked in getaddrinfo() or an mbedTLS
 * handshake on a captive-portal network routinely outlives any fixed sleep;
 * when it woke up it found ml, ml->events and the DERP TX queue already freed
 * and reused, producing use-after-free crashes on real hardware.
 *
 * Waiting forever is not an option either — the caller is usually trying to
 * bring a *new* instance up. So a stop that fails to join hands the context
 * to the orphan list instead: nothing is freed, the stale workers keep
 * running against memory that stays valid, and whichever of
 * microlink_init() / microlink_destroy() / microlink_reap_orphans() runs
 * next releases it once the mask finally reaches zero. Deferring a free is
 * cheap; a use-after-free is a panic and a reboot.
 * Adapted from cplewes/microlink@a415d646.
 * ========================================================================== */

/* How long microlink_stop() waits for the workers before giving up on the
 * join and letting the context be orphaned. Long enough to cover a DERP TLS
 * handshake timing out (10 s SO_RCVTIMEO) but not so long that a reconnect
 * stalls behind it. */
#ifndef ML_STOP_JOIN_TIMEOUT_MS
#define ML_STOP_JOIN_TIMEOUT_MS 10000
#endif
#define ML_JOIN_POLL_MS 20

/* Contexts that could not be freed at destroy time, awaiting their last
 * worker. One slot per outstanding teardown; in practice never more than
 * one, since each teardown is followed by a reap. */
#define ML_MAX_ORPHANS 4
static microlink_t *s_orphans[ML_MAX_ORPHANS];
static portMUX_TYPE s_orphan_mux = portMUX_INITIALIZER_UNLOCKED;

static void ml_release(microlink_t *ml);

uint32_t ml_tasks_running(const microlink_t *ml) {
    if (!ml) return 0;
    return __atomic_load_n(&ml->tasks_running, __ATOMIC_ACQUIRE);
}

bool ml_shutdown_pending(microlink_t *ml) {
    if (!ml || !ml->events) return true;
    return (xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST) != 0;
}

void ml_task_exit(microlink_t *ml, uint32_t task_bit) {
    if (ml) {
        __atomic_fetch_and(&ml->tasks_running, ~task_bit, __ATOMIC_RELEASE);
    }
    /* ml, ml->events and every ml-owned queue are off limits from here on:
     * clearing the bit above is exactly what licenses another task to free
     * them, possibly before the vTaskDelete() below has even run. */
    vTaskDelete(NULL);
    for (;;) { vTaskDelay(portMAX_DELAY); }  /* not reached */
}

static void ml_describe_tasks(uint32_t mask, char *out, size_t out_len) {
    snprintf(out, out_len, "%s%s%s%s",
             (mask & ML_TASK_BIT_NET_IO)  ? " net_io"  : "",
             (mask & ML_TASK_BIT_DERP_TX) ? " derp_tx" : "",
             (mask & ML_TASK_BIT_COORD)   ? " coord"   : "",
             (mask & ML_TASK_BIT_WG_MGR)  ? " wg_mgr"  : "");
}

/* Poll the liveness mask until it clears or the deadline passes. Returns the
 * mask of tasks still running (0 == fully joined). */
static uint32_t ml_join_tasks(microlink_t *ml, uint32_t timeout_ms) {
    uint32_t waited_ms = 0;
    uint32_t alive;

    while ((alive = ml_tasks_running(ml)) != 0 && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(ML_JOIN_POLL_MS));
        waited_ms += ML_JOIN_POLL_MS;
    }
    return alive;
}

static void ml_orphan_park(microlink_t *ml) {
    int slot = -1;

    portENTER_CRITICAL(&s_orphan_mux);
    for (int i = 0; i < ML_MAX_ORPHANS; i++) {
        if (!s_orphans[i]) { s_orphans[i] = ml; slot = i; break; }
    }
    portEXIT_CRITICAL(&s_orphan_mux);

    if (slot < 0) {
        /* Every slot taken means several teardowns in a row all left workers
         * behind — the network is badly wedged. Leaking permanently is still
         * the correct trade against a use-after-free. */
        ESP_LOGE(TAG, "orphan table full; context %p leaked permanently", ml);
    } else {
        ESP_LOGW(TAG, "context %p parked in orphan slot %d until its workers exit",
                 ml, slot);
    }
}

void microlink_reap_orphans(void) {
    for (int i = 0; i < ML_MAX_ORPHANS; i++) {
        microlink_t *ripe = NULL;

        portENTER_CRITICAL(&s_orphan_mux);
        if (s_orphans[i] && ml_tasks_running(s_orphans[i]) == 0) {
            ripe = s_orphans[i];
            s_orphans[i] = NULL;
        }
        portEXIT_CRITICAL(&s_orphan_mux);

        if (ripe) {
            ESP_LOGW(TAG, "reaping orphaned context %p (workers finally exited)", ripe);
            ml_release(ripe);
        }
    }
}

/* Free everything the context owns. Precondition: ml_tasks_running(ml) == 0.
 * Called either straight from microlink_destroy() or later from the reaper. */
static void ml_release(microlink_t *ml) {
    /* Deinitialize HTTP config server */
    if (ml->config_httpd) {
        ml_config_httpd_deinit(ml->config_httpd);
        ml->config_httpd = NULL;
    }

    /* Sockets microlink_stop() had to leave open because a worker was still
     * inside select()/recv() on them, plus the per-task sockets if their
     * owner exited without getting to its own cleanup. */
#ifdef CONFIG_ML_ZERO_COPY_WG
    ml_zerocopy_deinit(ml);
#endif
    if (ml->disco_sock4 >= 0)  { ml_close_sock(ml->disco_sock4);  ml->disco_sock4 = -1; }
    if (ml->stun_sock >= 0)    { ml_close_sock(ml->stun_sock);    ml->stun_sock = -1; }
    if (ml->stun_sock6 >= 0)   { ml_close_sock(ml->stun_sock6);   ml->stun_sock6 = -1; }
    if (ml->coord_sock >= 0)   { ml_close_sock(ml->coord_sock);   ml->coord_sock = -1; }
    if (ml->derp.sockfd >= 0)  { ml_close_sock(ml->derp.sockfd);  ml->derp.sockfd = -1; }

    if (ml->derp_tx_queue)    { vQueueDelete(ml->derp_tx_queue);    ml->derp_tx_queue    = NULL; }
    if (ml->disco_rx_queue)   { vQueueDelete(ml->disco_rx_queue);   ml->disco_rx_queue   = NULL; }
    if (ml->wg_rx_queue)      { vQueueDelete(ml->wg_rx_queue);      ml->wg_rx_queue      = NULL; }
    if (ml->stun_rx_queue)    { vQueueDelete(ml->stun_rx_queue);    ml->stun_rx_queue    = NULL; }
    if (ml->coord_cmd_queue)  { vQueueDelete(ml->coord_cmd_queue);  ml->coord_cmd_queue  = NULL; }
    if (ml->peer_update_queue){ vQueueDelete(ml->peer_update_queue);ml->peer_update_queue= NULL; }

    if (ml->events)           { vEventGroupDelete(ml->events);      ml->events           = NULL; }

    /* Clear keys from memory */
    memset(ml->machine_private_key, 0, 32);
    memset(ml->wg_private_key, 0, 32);
    memset(ml->disco_private_key, 0, 32);

    free(ml);
    ESP_LOGI(TAG, "Destroyed");
}

/* ============================================================================
 * Public API
 * ========================================================================== */

microlink_t *microlink_init(const microlink_config_t *config) {
    if (!config || !config->auth_key) {
        ESP_LOGE(TAG, "Invalid config: auth_key required");
        return NULL;
    }

    /* Reclaim any earlier context whose workers have drained since the last
     * teardown, before allocating another one. */
    microlink_reap_orphans();

    /* Route cJSON to PSRAM */
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn = free
    };
    cJSON_InitHooks(&hooks);

    /* Allocate context from PSRAM */
    microlink_t *ml = ml_psram_calloc(1, sizeof(microlink_t));
    if (!ml) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    /* Copy config */
    ml->config = *config;
    if (ml->config.max_peers == 0) ml->config.max_peers = ML_MAX_PEERS;
    if (ml->config.max_peers > ML_MAX_PEERS) ml->config.max_peers = ML_MAX_PEERS;
    ml->config.enable_derp = true;  /* Always need DERP for relay */

    ml->state = ML_STATE_IDLE;
    ml->coord_sock = -1;
    ml->disco_sock4 = -1;
    ml->disco_sock6 = -1;
    ml->stun_sock = -1;
    ml->stun_sock6 = -1;
    ml->derp.sockfd = -1;

    /* Resolve timing (0 = use defaults from #defines) */
    ml->t_disco_heartbeat_ms = ml->config.disco_heartbeat_ms ? ml->config.disco_heartbeat_ms : ML_DISCO_HEARTBEAT_MS;
    ml->t_stun_interval_ms = ml->config.stun_interval_ms ? ml->config.stun_interval_ms : ML_STUN_RESTUN_INTERVAL_MS;
    ml->t_ctrl_watchdog_ms = ml->config.ctrl_watchdog_ms ? ml->config.ctrl_watchdog_ms : ML_CTRL_WATCHDOG_MS;

    /* Apply Kconfig priority peer if set and app didn't provide one */
    if (ml->config.priority_peer_ip == 0 && strlen(CONFIG_ML_PRIORITY_PEER_IP) > 0) {
        ml->config.priority_peer_ip = microlink_parse_ip(CONFIG_ML_PRIORITY_PEER_IP);
        if (ml->config.priority_peer_ip) {
            ESP_LOGI(TAG, "Priority peer from Kconfig: %s", CONFIG_ML_PRIORITY_PEER_IP);
        }
    }

    /* Load or generate persistent keys */
    if (load_or_generate_keys(ml) != ESP_OK) {
        free(ml);
        return NULL;
    }

    /* Initialize peer NVS cache */
    ml_peer_nvs_init();

    /* Initialize HTTP config server (loads NVS peer allowlist + settings) */
    ml->config_httpd = ml_config_httpd_init();

    /* Override config with NVS-saved settings (web UI save → restart flow).
     * NVS settings take priority over Kconfig defaults.  Strings are copied
     * into ml->nvs_* buffers so the const char* pointers remain valid. */
    if (ml->config_httpd) {
        const char *nvs_auth = ml_config_get_auth_key(ml->config_httpd);
        if (nvs_auth) {
            strncpy(ml->nvs_auth_key, nvs_auth, sizeof(ml->nvs_auth_key) - 1);
            ml->config.auth_key = ml->nvs_auth_key;
            ESP_LOGI(TAG, "Auth key overridden from NVS (len=%d)", (int)strlen(nvs_auth));
        }
        /* Device name: full name takes priority, then prefix+MAC, then Kconfig */
        const char *nvs_full_name = ml_config_get_device_name_full(ml->config_httpd);
        if (nvs_full_name) {
            /* Full custom hostname (e.g. "sensor-tailscale") */
            strncpy(ml->nvs_device_name, nvs_full_name, sizeof(ml->nvs_device_name) - 1);
            ml->config.device_name = ml->nvs_device_name;
            ESP_LOGI(TAG, "Device name from NVS (full): %s", ml->nvs_device_name);
        } else {
            const char *nvs_prefix = ml_config_get_device_prefix(ml->config_httpd);
            if (nvs_prefix) {
                /* Device name = prefix + MAC suffix (e.g. "sensor-a1b2c3") */
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                snprintf(ml->nvs_device_name, sizeof(ml->nvs_device_name),
                         "%s-%02x%02x%02x", nvs_prefix, mac[3], mac[4], mac[5]);
                ml->config.device_name = ml->nvs_device_name;
                ESP_LOGI(TAG, "Device name from NVS (prefix): %s", ml->nvs_device_name);
            }
        }

        /* v2 overrides */
        uint8_t nvs_max = ml_config_get_max_peers(ml->config_httpd);
        if (nvs_max > 0 && nvs_max <= ML_MAX_PEERS) {
            ml->config.max_peers = nvs_max;
            ESP_LOGI(TAG, "Max peers overridden from NVS: %d", nvs_max);
        }
        uint16_t nvs_hb = ml_config_get_disco_heartbeat_ms(ml->config_httpd);
        if (nvs_hb > 0 && nvs_hb <= 60000) {
            ml->config.disco_heartbeat_ms = nvs_hb;
            ml->t_disco_heartbeat_ms = nvs_hb;
            ESP_LOGI(TAG, "DISCO heartbeat overridden from NVS: %dms", nvs_hb);
        }
        uint32_t nvs_pip = ml_config_get_priority_peer_ip(ml->config_httpd);
        if (nvs_pip > 0) {
            ml->config.priority_peer_ip = nvs_pip;
            char pip_str[16];
            microlink_ip_to_str(nvs_pip, pip_str);
            ESP_LOGI(TAG, "Priority peer overridden from NVS: %s", pip_str);
        }
        const char *nvs_ctrl = ml_config_get_ctrl_host(ml->config_httpd);
        if (nvs_ctrl) {
            strncpy(ml->ctrl_host, nvs_ctrl, sizeof(ml->ctrl_host) - 1);
            ESP_LOGI(TAG, "Control plane overridden from NVS: %s", ml->ctrl_host);
        }
        ml->debug_flags = ml_config_get_debug_flags(ml->config_httpd);
        if (ml->debug_flags) {
            ESP_LOGI(TAG, "Debug flags from NVS: 0x%02x", ml->debug_flags);
        }
    }

    /* Create event group */
    ml->events = xEventGroupCreate();
    if (!ml->events) {
        ESP_LOGE(TAG, "Failed to create event group");
        free(ml);
        return NULL;
    }

    /* Create queues */
    ml->derp_tx_queue = xQueueCreate(ML_DERP_TX_QUEUE_DEPTH, sizeof(ml_derp_tx_item_t));
    ml->disco_rx_queue = xQueueCreate(ML_DISCO_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->wg_rx_queue = xQueueCreate(ML_WG_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->stun_rx_queue = xQueueCreate(ML_STUN_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->coord_cmd_queue = xQueueCreate(ML_COORD_CMD_QUEUE_DEPTH, sizeof(ml_coord_cmd_t));
    ml->peer_update_queue = xQueueCreate(ML_PEER_UPDATE_QUEUE_DEPTH, sizeof(ml_peer_update_t *));

    if (!ml->derp_tx_queue || !ml->disco_rx_queue || !ml->wg_rx_queue ||
        !ml->stun_rx_queue || !ml->coord_cmd_queue || !ml->peer_update_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        microlink_destroy(ml);
        return NULL;
    }

    ESP_LOGI(TAG, "MicroLink v2 initialized (max_peers=%d)", ml->config.max_peers);
    return ml;
}

esp_err_t microlink_start(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;
    if (ml->state != ML_STATE_IDLE) {
        ESP_LOGW(TAG, "Already started (state=%d)", ml->state);
        return ESP_ERR_INVALID_STATE;
    }
    /* Restarting a context whose previous workers are still draining would
     * give two generations of tasks the same liveness bits, and the first one
     * to exit would clear the bit the other is still relying on. Make the
     * caller build a fresh context instead. */
    uint32_t stragglers = ml_tasks_running(ml);
    if (stragglers) {
        char names[48];
        ml_describe_tasks(stragglers, names, sizeof(names));
        ESP_LOGE(TAG, "Refusing to start: previous workers still running:%s", names);
        return ESP_ERR_INVALID_STATE;
    }

    ml->state = ML_STATE_WIFI_WAIT;
    xEventGroupClearBits(ml->events, ML_EVT_SHUTDOWN_REQUEST);

    /* Set WiFi TX power if configured */
    if (ml->config.wifi_tx_power_dbm > 0) {
        int8_t power_quarter_dbm = ml->config.wifi_tx_power_dbm * 4;
        esp_wifi_set_max_tx_power(power_quarter_dbm);
        ESP_LOGI(TAG, "WiFi TX power set to %d dBm", ml->config.wifi_tx_power_dbm);
    }

#ifdef CONFIG_ML_ZERO_COPY_WG
    /* Zero-copy mode: raw lwIP PCB replaces BSD socket for DISCO/WG UDP.
     * WG packets go directly to wireguardif_network_rx() from tcpip_thread.
     * DISCO packets go to SPSC ring buffer, STUN to existing queue. */
    if (ml_zerocopy_init(ml) != ESP_OK) {
        ESP_LOGE(TAG, "Zero-copy init failed, falling back to BSD socket");
        goto bsd_socket_fallback;
    }
    ESP_LOGI(TAG, "Zero-copy WG mode active (high-throughput)");
    goto skip_bsd_socket;

bsd_socket_fallback:
#endif
    /* Create DISCO/magicsock UDP socket (port 51820 = WireGuard standard)
     * This is the single socket for ALL direct UDP traffic: DISCO pings/pongs,
     * CallMeMaybe probes, and WireGuard encrypted data.
     * Matches v1 microlink_disco_init() and tailscale's pconn4. */
    ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->disco_sock4 >= 0) {
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(51820),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind port 51820 (errno=%d), trying ephemeral", errno);
            bind_addr.sin_port = 0;
            ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        }

        /* Mark packets as DSCP 46 (Expedited Forwarding) → WMM AC_VO.
         * WiFi APs with WMM use shorter contention windows for voice-priority
         * traffic, reducing jitter on WireGuard/DISCO UDP packets. */
        int tos = 0xB8;  /* DSCP 46 = EF, TOS byte = 46 << 2 = 184 */
        setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        /* Set non-blocking for select() in net_io */
        int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
        ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

        /* Record the actual bound port (getsockname not wrapped — AT sockets use stored port) */
        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        getsockname(ml->disco_sock4, (struct sockaddr *)&local_addr, &addr_len);
        ml->disco_local_port = ntohs(local_addr.sin_port);
        ESP_LOGI(TAG, "DISCO/magicsock UDP socket bound to port %d", ml->disco_local_port);
    } else {
        ESP_LOGE(TAG, "Failed to create DISCO socket: errno=%d", errno);
    }
#ifdef CONFIG_ML_ZERO_COPY_WG
skip_bsd_socket:
    ;
#endif

    /* Create tasks. If any create fails (most commonly OOM in internal RAM
     * when overall heap is low), we MUST clean up any tasks that already
     * came up before returning — otherwise they become orphans accessing
     * state that microlink_destroy() will later free. Each bit goes up
     * BEFORE its xTaskCreate: from the instant the task exists it may be
     * holding the context, and only it may clear the bit again. On a create
     * failure we take the bit back down here — nobody else can, because no
     * task was born to do it. */
    BaseType_t ret;
    const char *failed_task = NULL;
    uint32_t failed_bit = 0;

    __atomic_fetch_or(&ml->tasks_running, ML_TASK_BIT_NET_IO, __ATOMIC_RELEASE);
    ret = xTaskCreatePinnedToCore(ml_net_io_task, "ml_net_io", ML_TASK_NET_IO_STACK,
                                   ml, ML_TASK_NET_IO_PRIO, &ml->net_io_task, ML_TASK_NET_IO_CORE);
    if (ret != pdPASS) { failed_task = "ml_net_io";  failed_bit = ML_TASK_BIT_NET_IO;  goto fail_start; }

    __atomic_fetch_or(&ml->tasks_running, ML_TASK_BIT_DERP_TX, __ATOMIC_RELEASE);
    ret = xTaskCreatePinnedToCore(ml_derp_tx_task, "ml_derp_tx", ML_TASK_DERP_TX_STACK,
                                   ml, ML_TASK_DERP_TX_PRIO, &ml->derp_tx_task, ML_TASK_DERP_TX_CORE);
    if (ret != pdPASS) { failed_task = "ml_derp_tx"; failed_bit = ML_TASK_BIT_DERP_TX; goto fail_start; }

    __atomic_fetch_or(&ml->tasks_running, ML_TASK_BIT_COORD, __ATOMIC_RELEASE);
    ret = xTaskCreatePinnedToCore(ml_coord_task, "ml_coord", ML_TASK_COORD_STACK,
                                   ml, ML_TASK_COORD_PRIO, &ml->coord_task, ML_TASK_COORD_CORE);
    if (ret != pdPASS) { failed_task = "ml_coord";   failed_bit = ML_TASK_BIT_COORD;   goto fail_start; }

    __atomic_fetch_or(&ml->tasks_running, ML_TASK_BIT_WG_MGR, __ATOMIC_RELEASE);
    ret = xTaskCreatePinnedToCore(ml_wg_mgr_task, "ml_wg_mgr", ML_TASK_WG_MGR_STACK,
                                   ml, ML_TASK_WG_MGR_PRIO, &ml->wg_mgr_task, ML_TASK_WG_MGR_CORE);
    if (ret != pdPASS) { failed_task = "ml_wg_mgr";  failed_bit = ML_TASK_BIT_WG_MGR;  goto fail_start; }

    /* WiFi is expected to be connected before microlink_start() is called.
     * Signal the event so coord/wg_mgr tasks proceed immediately. */
    xEventGroupSetBits(ml->events, ML_EVT_WIFI_CONNECTED);

    /* Signal coord task to start connecting */
    ml_coord_cmd_t cmd = ML_CMD_CONNECT;
    xQueueSend(ml->coord_cmd_queue, &cmd, 0);

    /* Start HTTP config server (binds port 80, serves config page + REST API) */
    if (ml->config_httpd) {
        ml_config_httpd_start(ml->config_httpd, ml);
    }

    ESP_LOGI(TAG, "All tasks started");
    return ESP_OK;

fail_start:
    /* A task creation failed. The tasks we DID manage to create are now
     * running and would otherwise orphan-access state on the next destroy.
     * Signal shutdown and let microlink_stop() join them before returning to
     * the caller, so the partial-init goes back to a clean ML_STATE_IDLE that
     * the caller can microlink_destroy() safely.
     *
     * Diagnostic dump tells you which task fell over and how starved heap
     * was at the moment of failure — usually internal RAM exhaustion. */
    __atomic_fetch_and(&ml->tasks_running, ~failed_bit, __ATOMIC_RELEASE);
    ESP_LOGE(TAG, "Failed to create %s task (free internal=%u psram=%u); "
                  "rolling back partial init",
             failed_task,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* microlink_stop() sets the shutdown bit and joins whatever came up. If
     * one of them is wedged it stays in the liveness mask and destroy will
     * defer the free rather than pull the context out from under it. */
    microlink_stop(ml);
    return ESP_FAIL;
}

esp_err_t microlink_rebind(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;
    if (ml->state == ML_STATE_IDLE) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "=== Rebinding to new network interface ===");

    /* Step 1: Invalidate socket FDs FIRST, then delay to let net_io_task's
     * select() cycle complete (50ms timeout). Only THEN close the old FDs.
     * Closing a socket while another thread has it in select() deadlocks
     * on lwIP's global socket lock. */
    int old_disco = ml->disco_sock4;
    int old_stun = ml->stun_sock;
    int old_stun6 = ml->stun_sock6;

    /* Invalidate — net_io_task will skip these on next iteration */
    ml->disco_sock4 = -1;
    ml->stun_sock = -1;
    ml->stun_sock6 = -1;

    /* Wait for net_io_task select() to cycle out (50ms timeout + margin) */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Now safe to close the old FDs */
    if (old_disco >= 0) ml_close_sock(old_disco);
    if (old_stun >= 0) ml_close_sock(old_stun);
    if (old_stun6 >= 0) ml_close_sock(old_stun6);
    ESP_LOGI(TAG, "Rebind: closed DISCO + STUN sockets");

    /* Step 2: Signal coord to reconnect. ML_CMD_FORCE_RECONNECT closes
     * the coord socket, resets Noise state, and re-enters the
     * STUN → DNS → TCP → Noise → Register → MapRequest flow.
     * Peers and WG state are preserved. */
    xEventGroupClearBits(ml->events, ML_EVT_COORD_REGISTERED);
    ml_coord_cmd_t cmd = ML_CMD_FORCE_RECONNECT;
    xQueueSend(ml->coord_cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Step 3: Signal DERP to reconnect. ML_EVT_DERP_RECONNECT triggers
     * derp_tx_task to close TLS, then reconnect with full handshake.
     * Pending TX packets are drained but WG state is preserved. */
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);
    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
    ESP_LOGI(TAG, "Rebind: signaled coord + DERP to reconnect");

    /* Step 4: Brief delay for coord/DERP to process reconnect signals */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 5: Recreate DISCO UDP socket on the new interface */
#ifdef CONFIG_ML_ZERO_COPY_WG
    ml_zerocopy_deinit(ml);
    if (ml_zerocopy_init(ml) == ESP_OK) {
        ESP_LOGI(TAG, "Rebind: zero-copy DISCO PCB recreated");
        goto rebind_wg_update;
    }
    ESP_LOGW(TAG, "Rebind: zero-copy init failed, using BSD socket fallback");
#endif
    ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->disco_sock4 >= 0) {
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(51820),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            bind_addr.sin_port = 0;
            ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        }
        int tos = 0xB8;
        setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
        ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        getsockname(ml->disco_sock4, (struct sockaddr *)&local_addr, &addr_len);
        ml->disco_local_port = ntohs(local_addr.sin_port);
        ESP_LOGI(TAG, "Rebind: DISCO socket rebound to port %d", ml->disco_local_port);
    } else {
        ESP_LOGE(TAG, "Rebind: failed to create DISCO socket: errno=%d", errno);
    }

#ifdef CONFIG_ML_ZERO_COPY_WG
rebind_wg_update:
#endif
    /* Step 6: Update WG output mode for new transport */
    ml_wg_mgr_update_transport(ml);

    ESP_LOGI(TAG, "=== Rebind complete — waiting for coord+DERP reconnect ===");
    return ESP_OK;
}

esp_err_t microlink_stop(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Stopping...");
    if (ml->events) {
        xEventGroupSetBits(ml->events, ML_EVT_SHUTDOWN_REQUEST);
    }

    /* Join the workers for real. Each one clears its ML_TASK_BIT_* as its
     * last act, so a zero mask proves none of them can reach this context
     * again — see the teardown contract above. Tasks self-delete, so we
     * never call vTaskDelete() on them from here. */
    uint32_t alive = ml_join_tasks(ml, ML_STOP_JOIN_TIMEOUT_MS);
    char names[48];
    ml_describe_tasks(alive, names, sizeof(names));

    if (alive) {
        ESP_LOGE(TAG, "Join timed out after %d ms; still running:%s (mask 0x%02x). "
                      "Context will be held until they exit.",
                 ML_STOP_JOIN_TIMEOUT_MS, names, (unsigned)alive);
    }

    /* A handle is only meaningful while its task exists; clear the ones we
     * have proof are gone and keep the rest for diagnostics. */
    if (!(alive & ML_TASK_BIT_NET_IO))  ml->net_io_task  = NULL;
    if (!(alive & ML_TASK_BIT_DERP_TX)) ml->derp_tx_task = NULL;
    if (!(alive & ML_TASK_BIT_COORD))   ml->coord_task   = NULL;
    if (!(alive & ML_TASK_BIT_WG_MGR))  ml->wg_mgr_task  = NULL;

    /* Stop HTTP config server */
    if (ml->config_httpd) {
        ml_config_httpd_stop(ml->config_httpd);
    }

    /* The DISCO/STUN sockets are the ones net_io holds in select(); closing
     * a descriptor out from under a live select() is the lwIP deadlock that
     * microlink_rebind() goes out of its way to avoid, so only close them
     * once net_io is provably gone. Freeing them promptly matters — the next
     * instance wants to bind port 51820 again. If net_io is the straggler,
     * ml_release() closes them at reap time instead. */
    if (!(alive & ML_TASK_BIT_NET_IO)) {
#ifdef CONFIG_ML_ZERO_COPY_WG
        ml_zerocopy_deinit(ml);
#endif
        if (ml->disco_sock4 >= 0) { ml_close_sock(ml->disco_sock4); ml->disco_sock4 = -1; }
        if (ml->stun_sock >= 0)   { ml_close_sock(ml->stun_sock);   ml->stun_sock = -1; }
        if (ml->stun_sock6 >= 0)  { ml_close_sock(ml->stun_sock6);  ml->stun_sock6 = -1; }
    }

    ml->state = ML_STATE_IDLE;
    ESP_LOGI(TAG, "Stopped%s", alive ? " (workers still draining)" : "");
    return alive ? ESP_ERR_TIMEOUT : ESP_OK;
}

void microlink_destroy(microlink_t *ml) {
    if (!ml) return;

    microlink_stop(ml);

    /* Opportunistic: release anything parked by an earlier teardown that has
     * drained in the meantime. */
    microlink_reap_orphans();

    uint32_t alive = ml_tasks_running(ml);
    if (alive) {
        /* Freeing now is precisely the bug this contract exists to prevent:
         * the straggler wakes up from its socket call and dereferences ml,
         * ml->events or the DERP TX queue. Park the context instead. */
        char names[48];
        ml_describe_tasks(alive, names, sizeof(names));
        ESP_LOGE(TAG, "Deferring free:%s still running (mask 0x%02x)", names, (unsigned)alive);
        ml_orphan_park(ml);
        return;
    }

    /* Peer NVS is a process-wide handle, not per-instance — only close it on
     * the path where this instance really is the last one standing. */
    ml_peer_nvs_deinit();
    ml_release(ml);
}

/* ============================================================================
 * State Queries
 * ========================================================================== */

microlink_state_t microlink_get_state(const microlink_t *ml) {
    return ml ? ml->state : ML_STATE_IDLE;
}

bool microlink_is_connected(const microlink_t *ml) {
    return ml && ml->state == ML_STATE_CONNECTED;
}

uint32_t microlink_get_vpn_ip(const microlink_t *ml) {
    return ml ? ml->vpn_ip : 0;
}

int microlink_get_peer_count(const microlink_t *ml) {
    return ml ? ml->peer_count : 0;
}

esp_err_t microlink_get_peer_info(const microlink_t *ml, int index, microlink_peer_info_t *info) {
    if (!ml || !info || index < 0 || index >= ml->peer_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const ml_peer_t *p = &ml->peers[index];
    info->vpn_ip = p->vpn_ip;
    strncpy(info->hostname, p->hostname, sizeof(info->hostname) - 1);
    memcpy(info->public_key, p->public_key, 32);
    info->online = p->active;
    info->direct_path = p->has_direct_path;
    return ESP_OK;
}

/* ============================================================================
 * Send API
 * ========================================================================== */

esp_err_t microlink_send(microlink_t *ml, uint32_t dest_vpn_ip,
                          const uint8_t *data, size_t len) {
    if (!ml || !data || len == 0 || len > 1400) return ESP_ERR_INVALID_ARG;
    if (ml->state != ML_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;

    /* Find peer by VPN IP */
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].vpn_ip == dest_vpn_ip && ml->peers[i].active) {
            /* TODO: Route through WireGuard tunnel */
            /* For now, queue via DERP as fallback */
            return ml_derp_queue_send(ml, ml->peers[i].public_key, data, len);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ============================================================================
 * Callbacks
 * ========================================================================== */

void microlink_set_state_callback(microlink_t *ml, microlink_state_cb_t cb, void *user_data) {
    if (ml) { ml->state_cb = cb; ml->state_cb_data = user_data; }
}

void microlink_set_peer_callback(microlink_t *ml, microlink_peer_cb_t cb, void *user_data) {
    if (ml) { ml->peer_cb = cb; ml->peer_cb_data = user_data; }
}

void microlink_set_data_callback(microlink_t *ml, microlink_data_cb_t cb, void *user_data) {
    if (ml) { ml->data_cb = cb; ml->data_cb_data = user_data; }
}

/* ============================================================================
 * Utilities
 * ========================================================================== */

void microlink_ip_to_str(uint32_t ip, char *buf) {
    snprintf(buf, 16, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF),
             (unsigned long)((ip >> 8) & 0xFF),
             (unsigned long)(ip & 0xFF));
}

uint32_t microlink_parse_ip(const char *ip_str) {
    if (!ip_str) return 0;
    unsigned int a, b, c, d;
    if (sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

const char *microlink_default_device_name(void) {
    static char name[48] = {0};
    if (name[0] == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        /* Use ML_DEVICE_NAME as prefix if configured, otherwise "esp32" */
        const char *prefix = CONFIG_ML_DEVICE_NAME;
        if (!prefix || !prefix[0]) prefix = "esp32";
        snprintf(name, sizeof(name), "%s-%02x%02x%02x", prefix, mac[3], mac[4], mac[5]);
    }
    return name;
}

const char *microlink_imei_device_name(void) {
#ifdef CONFIG_ML_ENABLE_CELLULAR
    static char name[48] = {0};  /* prefix + "-" + 15-digit IMEI + null */
    const char *imei = ml_cellular_get_imei();
    if (imei && imei[0]) {
        const char *prefix = CONFIG_ML_DEVICE_NAME;
        if (!prefix || !prefix[0]) prefix = "esp32";
        snprintf(name, sizeof(name), "%s-%s", prefix, imei);
        return name;
    }
#endif
    return NULL;
}

uint64_t ml_get_time_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

/* ============================================================================
 * MagicDNS — Resolve tailnet hostnames against peer list
 * ========================================================================== */

/* Case-insensitive string compare (limited to len bytes) */
static int strncasecmp_local(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

uint32_t microlink_resolve(const microlink_t *ml, const char *hostname) {
    if (!ml || !hostname || hostname[0] == '\0') return 0;

    size_t query_len = strlen(hostname);

    for (int i = 0; i < ml->peer_count; i++) {
        const ml_peer_t *p = &ml->peers[i];
        if (!p->active || p->hostname[0] == '\0') continue;

        /* 1. Exact match (case-insensitive) */
        if (strncasecmp_local(p->hostname, hostname, sizeof(p->hostname)) == 0) {
            return p->vpn_ip;
        }

        /* 2. Prefix match: query "npc1" matches peer "npc1.tail12345.ts.net"
         * The query must match up to the first '.' in the peer hostname. */
        const char *dot = strchr(p->hostname, '.');
        if (dot) {
            size_t short_len = (size_t)(dot - p->hostname);
            if (query_len == short_len &&
                strncasecmp_local(p->hostname, hostname, short_len) == 0) {
                return p->vpn_ip;
            }
        }
    }

    return 0;  /* Not found */
}
