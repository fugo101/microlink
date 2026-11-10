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
 *   A shutdown handler writes them on every esp_restart().
 * - add_peer() restores the saved value with ml_wg_ts_nvs_load() before
 *   wireguardif_add_peer().
 *
 * Storage: blob "wgts1" in the "ml_peers" namespace, so ml_peer_nvs_clear() and
 * microlink_factory_reset() erase it with the peer cache. Kept separate from
 * the peer cache blob so that table's 92-byte entry layout is untouched.
 * Packed array of { 16-byte public key prefix, 12-byte TAI64N }. When full,
 * the entry with the oldest timestamp (longest since a handshake) is evicted.
 *
 * Timestamps from before 2020 are not saved: they come from a peer whose clock
 * was never set (e.g. an ESP32 without SNTP sends 1970 + uptime) and restarts
 * near zero on its reboot, so a saved value would lock it out until its uptime
 * caught up. Such peers keep the RAM-only behaviour.
 *
 * Locking: s_lock guards the table and the NVS handle (wg_mgr poll, shutdown
 * handler, clear). s_dirty_mask is lock-free so the rx path never blocks.
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "wireguardif.h"
#include "wireguard.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ml_wg_ts";

#define WG_TS_NVS_NAMESPACE     "ml_peers"  /* Shared with ml_peer_nvs.c */
#define WG_TS_BLOB_KEY          "wgts1"     /* Bump if the entry layout changes */
#define WG_TS_KEY_PREFIX_LEN    16
#define WG_TS_MAX_ENTRIES       (2 * WIREGUARD_MAX_PEERS)
#define WG_TS_FLUSH_INTERVAL_MS ((uint64_t)CONFIG_ML_WG_TS_FLUSH_INTERVAL_S * 1000)
/* TAI64 label of 2020-01-01 00:00:00 UTC (2^62 + 10 + unix seconds) */
#define WG_TS_MIN_SECONDS       (0x400000000000000AULL + 1577836800ULL)

_Static_assert(WIREGUARD_MAX_PEERS <= 32, "s_dirty_mask has one bit per WireGuard peer slot");
_Static_assert(ML_WG_TS_LEN == WIREGUARDIF_TIMESTAMP_LEN, "TAI64N timestamp length mismatch");

typedef struct __attribute__((packed)) {
    uint8_t key_prefix[WG_TS_KEY_PREFIX_LEN];   /* 16 bytes */
    uint8_t ts[ML_WG_TS_LEN];                   /* 12 bytes, big-endian TAI64N */
} wg_ts_entry_t;                                /* Total: 28 bytes */

static nvs_handle_t s_nvs = 0;
static bool s_initialized = false;
static const microlink_t *s_owner = NULL;  /* Instance whose wg_mgr may poll */
static wg_ts_entry_t s_entries[WG_TS_MAX_ENTRIES];
static int s_count = 0;
static bool s_table_dirty = false;      /* RAM table newer than flash */
static bool s_flushed = false;          /* First write after boot goes out immediately */
static uint64_t s_last_flush_ms = 0;
static uint32_t s_dirty_mask = 0;       /* Bit per WG peer slot, set from the rx path */
static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_buf;
static bool s_shutdown_registered = false;

static int find_entry(const uint8_t *key) {
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].key_prefix, key, WG_TS_KEY_PREFIX_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* Record ts for the peer (only the first 16 bytes of key are used) if it is
 * newer than what we have. Returns true if changed. */
static bool table_store(const uint8_t *key, const uint8_t *ts) {
    uint64_t seconds = 0;
    for (int b = 0; b < 8; b++) seconds = (seconds << 8) | ts[b];
    if (seconds < WG_TS_MIN_SECONDS) return false;  /* Unset peer clock (or zero) */

    int i = find_entry(key);
    if (i >= 0) {
        /* Never go backwards - matches wireguard's own check */
        if (memcmp(ts, s_entries[i].ts, ML_WG_TS_LEN) <= 0) return false;
    } else {
        if (s_count < WG_TS_MAX_ENTRIES) {
            i = s_count++;
        } else {
            /* Evict the oldest timestamp, unless this one is older still */
            i = 0;
            for (int j = 1; j < s_count; j++) {
                if (memcmp(s_entries[j].ts, s_entries[i].ts, ML_WG_TS_LEN) < 0) i = j;
            }
            if (memcmp(ts, s_entries[i].ts, ML_WG_TS_LEN) <= 0) return false;
        }
        memcpy(s_entries[i].key_prefix, key, WG_TS_KEY_PREFIX_LEN);
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

/* Caller holds s_lock */
static void table_flush(uint64_t now) {
    esp_err_t err = nvs_set_blob(s_nvs, WG_TS_BLOB_KEY, s_entries,
                                 s_count * sizeof(wg_ts_entry_t));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err == ESP_OK) {
        __atomic_store_n(&s_table_dirty, false, __ATOMIC_RELAXED);
        ESP_LOGD(TAG, "Saved handshake timestamps (%d peers)", s_count);
    } else {
        /* Keep it pending; retry after the interval rather than hammering flash */
        ESP_LOGW(TAG, "Saving handshake timestamps failed: %s", esp_err_to_name(err));
    }
    s_flushed = true;
    s_last_flush_ms = now;
}

/* Caller holds s_lock */
static void table_load(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;

    size_t len = 0;
    esp_err_t err = nvs_get_blob(s_nvs, WG_TS_BLOB_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || (err == ESP_OK && len == 0)) return;
    if (err != ESP_OK || len % sizeof(wg_ts_entry_t) != 0) {
        ESP_LOGW(TAG, "Ignoring saved handshake timestamps: %s len=%u",
                 esp_err_to_name(err), (unsigned)len);
        return;
    }

    if (len <= sizeof(s_entries)) {
        err = nvs_get_blob(s_nvs, WG_TS_BLOB_KEY, s_entries, &len);
        if (err == ESP_OK) {
            s_count = len / sizeof(wg_ts_entry_t);
        } else {
            ESP_LOGW(TAG, "Reading handshake timestamps failed: %s", esp_err_to_name(err));
            memset(s_entries, 0, sizeof(s_entries));
        }
        return;
    }

    /* Saved with a bigger table (e.g. a larger WIREGUARD_MAX_PEERS): keep the
     * newest entries that fit instead of dropping them all */
    wg_ts_entry_t *saved = malloc(len);
    if (!saved) {
        ESP_LOGW(TAG, "No memory to read %u bytes of handshake timestamps", (unsigned)len);
        return;
    }
    err = nvs_get_blob(s_nvs, WG_TS_BLOB_KEY, saved, &len);
    if (err == ESP_OK) {
        int n = len / sizeof(wg_ts_entry_t);
        for (int i = 0; i < n; i++) {
            table_store(saved[i].key_prefix, saved[i].ts);
        }
        ESP_LOGW(TAG, "Saved handshake timestamps truncated: %d -> %d peers", n, s_count);
    } else {
        ESP_LOGW(TAG, "Reading handshake timestamps failed: %s", esp_err_to_name(err));
    }
    free(saved);
}

/* Runs inside esp_restart() on the calling task (web UI restart, OTA, app):
 * write what the wg_mgr loop has already collected. Panic, watchdog and
 * power loss never get here. */
static void wg_ts_shutdown_handler(void) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (s_initialized && s_table_dirty) {
        table_flush(ml_get_time_ms());
    }
    xSemaphoreGive(s_lock);
}

esp_err_t ml_wg_ts_nvs_init(microlink_t *ml) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* A parked straggler from an earlier instance may still poll; only the
     * newest instance's wg_mgr owns the slot-indexed dirty mask */
    s_owner = ml;
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    esp_err_t err = nvs_open(WG_TS_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "NVS open failed: %d (handshake timestamps not persisted)", err);
        return err;
    }

    table_load();
    __atomic_store_n(&s_table_dirty, false, __ATOMIC_RELAXED);
    s_flushed = false;
    __atomic_store_n(&s_dirty_mask, 0, __ATOMIC_RELAXED);
    s_initialized = true;

    if (!s_shutdown_registered) {
        err = esp_register_shutdown_handler(wg_ts_shutdown_handler);
        if (err == ESP_OK) {
            s_shutdown_registered = true;
        } else {
            ESP_LOGW(TAG, "No shutdown handler (%s): restart may lose up to %ds of timestamps",
                     esp_err_to_name(err), CONFIG_ML_WG_TS_FLUSH_INTERVAL_S);
        }
    }
    int count = s_count;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Handshake timestamps loaded for %d peers (max %d)",
             count, WG_TS_MAX_ENTRIES);
    return ESP_OK;
}

void ml_wg_ts_nvs_deinit(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        nvs_close(s_nvs);
        s_initialized = false;
    }
    s_owner = NULL;
    xSemaphoreGive(s_lock);
}

bool ml_wg_ts_nvs_load(const uint8_t public_key[32], uint8_t ts[ML_WG_TS_LEN]) {
    if (!s_lock || !public_key || !ts) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = s_initialized ? find_entry(public_key) : -1;
    if (i >= 0) {
        memcpy(ts, s_entries[i].ts, ML_WG_TS_LEN);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

void ml_wg_ts_nvs_mark_dirty(uint8_t wg_peer_index) {
    if (wg_peer_index < WIREGUARD_MAX_PEERS) {
        __atomic_fetch_or(&s_dirty_mask, 1u << wg_peer_index, __ATOMIC_RELEASE);
    }
}

void ml_wg_ts_nvs_poll(microlink_t *ml, bool force) {
    if (!s_lock || !ml) return;
    /* Fast path every loop: nothing flagged and nothing waiting for flash */
    if (__atomic_load_n(&s_dirty_mask, __ATOMIC_RELAXED) == 0 &&
        !__atomic_load_n(&s_table_dirty, __ATOMIC_RELAXED)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized || ml != s_owner) {
        xSemaphoreGive(s_lock);
        return;
    }

    uint32_t mask = __atomic_exchange_n(&s_dirty_mask, 0, __ATOMIC_ACQUIRE);
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
            __atomic_store_n(&s_table_dirty, true, __ATOMIC_RELAXED);
        }
    }

    if (s_table_dirty) {
        uint64_t now = ml_get_time_ms();
        if (force || !s_flushed || now - s_last_flush_ms >= WG_TS_FLUSH_INTERVAL_MS) {
            table_flush(now);
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t ml_wg_ts_nvs_clear(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    __atomic_store_n(&s_dirty_mask, 0, __ATOMIC_RELAXED);
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    __atomic_store_n(&s_table_dirty, false, __ATOMIC_RELAXED);

    /* Not initialised: open the namespace just for this (e.g. before microlink_init) */
    nvs_handle_t h = s_nvs;
    esp_err_t err = s_initialized ? ESP_OK : nvs_open(WG_TS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, WG_TS_BLOB_KEY);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;  /* Never saved, or already gone with the namespace */
        }
        if (!s_initialized) nvs_close(h);
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return err;
}
