/**
 * @file ml_wg_ts_nvs.c
 * @brief WireGuard handshake timestamp persistence - replay protection across reboots
 *
 * WireGuard drops a handshake initiation whose TAI64N timestamp is not greater
 * than the greatest one already received from that peer (whitepaper 5.1), but
 * wireguard-lwip only keeps that value in RAM. Without saving it, a captured
 * initiation can be replayed once after every reboot (GHSA-7grw-r329-8g8c).
 *
 * Flow:
 * - remote_initiation_accepted runs on the rx path (wg_mgr task, or the tcpip
 *   thread in zero-copy mode) and only sets the peer's bit in s_dirty_mask.
 * - ml_wg_ts_nvs_poll() on the wg_mgr task copies dirty peers' timestamps into
 *   the RAM table and writes it to NVS at most once per
 *   CONFIG_ML_WG_TS_FLUSH_INTERVAL_S. Unwritten values stay pending, never dropped.
 * - add_peer() restores the saved value with ml_wg_ts_nvs_load() before
 *   wireguardif_add_peer().
 *
 * Storage: blob "wgts1" in the "ml_peers" namespace, so ml_peer_nvs_clear() and
 * microlink_factory_reset() erase it with the peer cache. Kept separate from
 * the peer cache blob so that table's 92-byte entry layout is untouched.
 * Packed array of { 16-byte public key prefix, 12-byte TAI64N }. When full,
 * the entry with the oldest timestamp (longest since a handshake) is evicted.
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "nvs.h"
#include "wireguardif.h"
#include "wireguard.h"
#include <string.h>

static const char *TAG = "ml_wg_ts";

#define WG_TS_NVS_NAMESPACE     "ml_peers"  /* Shared with ml_peer_nvs.c */
#define WG_TS_BLOB_KEY          "wgts1"     /* Bump if the entry layout changes */
#define WG_TS_KEY_PREFIX_LEN    16
#define WG_TS_MAX_ENTRIES       (2 * WIREGUARD_MAX_PEERS)
#define WG_TS_FLUSH_INTERVAL_MS ((uint64_t)CONFIG_ML_WG_TS_FLUSH_INTERVAL_S * 1000)

_Static_assert(WIREGUARD_MAX_PEERS <= 32, "s_dirty_mask has one bit per WireGuard peer slot");
_Static_assert(ML_WG_TS_LEN == WIREGUARDIF_TIMESTAMP_LEN, "TAI64N timestamp length mismatch");

typedef struct __attribute__((packed)) {
    uint8_t key_prefix[WG_TS_KEY_PREFIX_LEN];   /* 16 bytes */
    uint8_t ts[ML_WG_TS_LEN];                   /* 12 bytes, big-endian TAI64N */
} wg_ts_entry_t;                                /* Total: 28 bytes */

static nvs_handle_t s_nvs = 0;
static bool s_initialized = false;
static wg_ts_entry_t s_entries[WG_TS_MAX_ENTRIES];
static int s_count = 0;
static bool s_table_dirty = false;      /* RAM table newer than flash */
static bool s_flushed = false;          /* First write after boot goes out immediately */
static uint64_t s_last_flush_ms = 0;
static uint32_t s_dirty_mask = 0;       /* Bit per WG peer slot, set from the rx path */

static int find_entry(const uint8_t *public_key) {
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].key_prefix, public_key, WG_TS_KEY_PREFIX_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* Record ts for the peer if it is newer than what we have. Returns true if changed. */
static bool table_store(const uint8_t *public_key, const uint8_t *ts) {
    static const uint8_t zero[ML_WG_TS_LEN] = {0};
    if (memcmp(ts, zero, ML_WG_TS_LEN) == 0) return false;

    int i = find_entry(public_key);
    if (i >= 0) {
        /* Never go backwards - matches wireguard's own check */
        if (memcmp(ts, s_entries[i].ts, ML_WG_TS_LEN) <= 0) return false;
    } else {
        if (s_count < WG_TS_MAX_ENTRIES) {
            i = s_count++;
        } else {
            /* Evict the oldest timestamp */
            i = 0;
            for (int j = 1; j < s_count; j++) {
                if (memcmp(s_entries[j].ts, s_entries[i].ts, ML_WG_TS_LEN) < 0) i = j;
            }
        }
        memcpy(s_entries[i].key_prefix, public_key, WG_TS_KEY_PREFIX_LEN);
    }
    memcpy(s_entries[i].ts, ts, ML_WG_TS_LEN);
    return true;
}

/* The rx path may be writing greatest_timestamp on the tcpip thread while we
 * read it here without the lwIP lock. Accept a value only once two consecutive
 * reads agree. Returns 1 = ok, 0 = still changing (retry later), -1 = no peer. */
static int read_stable(struct netif *netif, uint8_t peer_index, uint8_t ts[ML_WG_TS_LEN]) {
    uint8_t again[ML_WG_TS_LEN];
    if (wireguardif_peer_greatest_timestamp(netif, peer_index, ts, ML_WG_TS_LEN) != ERR_OK) {
        return -1;
    }
    for (int tries = 0; tries < 3; tries++) {
        if (wireguardif_peer_greatest_timestamp(netif, peer_index, again, ML_WG_TS_LEN) != ERR_OK) {
            return -1;
        }
        if (memcmp(ts, again, ML_WG_TS_LEN) == 0) return 1;
        memcpy(ts, again, ML_WG_TS_LEN);
    }
    return 0;
}

static void table_flush(uint64_t now) {
    esp_err_t err = nvs_set_blob(s_nvs, WG_TS_BLOB_KEY, s_entries,
                                 s_count * sizeof(wg_ts_entry_t));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err == ESP_OK) {
        s_table_dirty = false;
        ESP_LOGD(TAG, "Saved handshake timestamps (%d peers)", s_count);
    } else {
        /* Keep it pending; retry after the interval rather than hammering flash */
        ESP_LOGW(TAG, "Saving handshake timestamps failed: %s", esp_err_to_name(err));
    }
    s_flushed = true;
    s_last_flush_ms = now;
}

esp_err_t ml_wg_ts_nvs_init(void) {
    if (s_initialized) return ESP_OK;

    esp_err_t err = nvs_open(WG_TS_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %d (handshake timestamps not persisted)", err);
        return err;
    }

    size_t len = sizeof(s_entries);
    err = nvs_get_blob(s_nvs, WG_TS_BLOB_KEY, s_entries, &len);
    if (err == ESP_OK && len % sizeof(wg_ts_entry_t) == 0) {
        s_count = len / sizeof(wg_ts_entry_t);
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Ignoring saved handshake timestamps: %s len=%u",
                     esp_err_to_name(err), (unsigned)len);
        }
        memset(s_entries, 0, sizeof(s_entries));
        s_count = 0;
    }
    s_table_dirty = false;
    s_flushed = false;
    __atomic_store_n(&s_dirty_mask, 0, __ATOMIC_RELAXED);
    s_initialized = true;

    ESP_LOGI(TAG, "Handshake timestamps loaded for %d peers (max %d)",
             s_count, WG_TS_MAX_ENTRIES);
    return ESP_OK;
}

void ml_wg_ts_nvs_deinit(void) {
    if (!s_initialized) return;
    nvs_close(s_nvs);
    s_initialized = false;
}

bool ml_wg_ts_nvs_load(const uint8_t public_key[32], uint8_t ts[ML_WG_TS_LEN]) {
    if (!s_initialized || !public_key || !ts) return false;
    int i = find_entry(public_key);
    if (i < 0) return false;
    memcpy(ts, s_entries[i].ts, ML_WG_TS_LEN);
    return true;
}

void ml_wg_ts_nvs_mark_dirty(uint8_t wg_peer_index) {
    if (wg_peer_index < WIREGUARD_MAX_PEERS) {
        __atomic_fetch_or(&s_dirty_mask, 1u << wg_peer_index, __ATOMIC_RELEASE);
    }
}

void ml_wg_ts_nvs_poll(microlink_t *ml, bool force) {
    if (!s_initialized || !ml) return;

    uint32_t mask = 0;
    if (__atomic_load_n(&s_dirty_mask, __ATOMIC_RELAXED) != 0) {
        mask = __atomic_exchange_n(&s_dirty_mask, 0, __ATOMIC_ACQUIRE);
    }
    for (int idx = 0; mask && idx < WIREGUARD_MAX_PEERS; idx++) {
        if (!(mask & (1u << idx))) continue;
        mask &= ~(1u << idx);
        if (!ml->wg_netif) break;

        /* Map WG slot -> ml peer; slot may have been freed since the callback */
        const ml_peer_t *p = NULL;
        for (int i = 0; i < ml->peer_count; i++) {
            if (ml->peers[i].active && ml->peers[i].wg_peer_index == idx) {
                p = &ml->peers[i];
                break;
            }
        }
        if (!p) continue;

        uint8_t ts[ML_WG_TS_LEN];
        int r = read_stable((struct netif *)ml->wg_netif, (uint8_t)idx, ts);
        if (r == 0) {
            ml_wg_ts_nvs_mark_dirty((uint8_t)idx);
        } else if (r > 0 && table_store(p->public_key, ts)) {
            s_table_dirty = true;
        }
    }

    if (!s_table_dirty) return;
    uint64_t now = ml_get_time_ms();
    if (force || !s_flushed || now - s_last_flush_ms >= WG_TS_FLUSH_INTERVAL_MS) {
        table_flush(now);
    }
}

esp_err_t ml_wg_ts_nvs_clear(void) {
    __atomic_store_n(&s_dirty_mask, 0, __ATOMIC_RELAXED);
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    s_table_dirty = false;
    if (!s_initialized) return ESP_OK;

    esp_err_t err = nvs_erase_key(s_nvs, WG_TS_BLOB_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  /* Already gone with the namespace */
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}
