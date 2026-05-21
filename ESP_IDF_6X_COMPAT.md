# ESP-IDF 6.x Compatibility Branch

Branch: `esp-idf-6x-compat` (rebased on top of upstream `main`)

Upstream MicroLink targets ESP-IDF v5.x. This branch adds patches to build cleanly on
**ESP-IDF 6.0+** (mbedTLS 4.x / TF-PSA-Crypto, GCC 15).

## Patches Applied

### 1. CMakeLists.txt — component name fix
`json` renamed to `espressif__cjson` in `IDFCOMPONENT_MANAGER` managed components.
ESP-IDF 6 registers the managed component under its full scoped name.

### 2. microlink_internal.h — remove private mbedTLS entropy headers
`mbedtls/entropy.h` and `mbedtls/ctr_drbg.h` moved to private headers in mbedTLS 4.x.
Removed `#include` directives and removed `mbedtls_entropy_context` / `mbedtls_ctr_drbg_context`
fields from `ml_derp_conn_t` — mbedTLS 4.x uses PSA RNG automatically, no manual entropy seeding needed.

### 3. ml_derp.c — remove entropy/CTR-DRBG init and ssl_conf_rng
`mbedtls_ssl_conf_rng()` deleted in mbedTLS 4.x (PSA handles RNG).
Removed all `mbedtls_entropy_init/seed/free` and `mbedtls_ctr_drbg_init/seed/free` calls.
Removed `mbedtls_ssl_conf_rng()` call.

### 4. ml_noise.c — redirect ChaCha20-Poly1305 header
`mbedtls/chachapoly.h` moved to private path in mbedTLS 4.x.
Changed to `mbedtls/private/chachapoly.h`. Removed unused `mbedtls/chacha20.h` include.

### 5. ml_wg_mgr.c + ml_peer_nvs.c — strncpy → memcpy
GCC 15 promotes `-Wstringop-truncation` to error.
Replaced `strncpy(dst, src, size-1)` with `memcpy(dst, src, size-1)` + explicit null-terminator.

### 6. CMakeLists.txt — conditional cellular compilation
`ml_cellular.c` and `ml_at_socket.c` guarded under `if(CONFIG_ML_ENABLE_CELLULAR)`.
These files include `driver/uart.h` which was split into `esp_driver_uart` in ESP-IDF 6.
Excluding them when cellular is disabled avoids the missing-header error.

### 7. nacl_box.c + wireguard.c — suppress unterminated string init warning
GCC 15 errors on C-style sigma constant initialized as `char[32] = "expand 32-byte k"` (32 chars, no null).
Added `-Wno-unterminated-string-initialization` via `set_source_files_properties` in CMakeLists.txt.

### 8. ml_wg_mgr.c + wireguardif.c — NULL-guard netif->state around netif link/up calls

ESP-IDF 6.x registers a global lwIP netif ext callback (`netif_callback_fn` in
`esp_netif_lwip.c`) that reads `netif->state` as `esp_netif_t*` on every
`netif_set_up`, `netif_set_link_up`, and `netif_set_link_down` call.
WireGuard stores `wireguard_device*` in `netif->state` after `wireguardif_init`.
On ESP-IDF 6, this misinterpretation causes a `LoadProhibited` crash (EXCCAUSE=0x1c)
when any of these functions is called on the WireGuard netif.

**Crash chain:** `netif_set_link_down/up` → `netif_invoke_ext_callback` →
`netif_callback_fn` → `lwip_get_esp_netif(netif)` reads `netif->state` as
`esp_netif_t*` → dereference of `wireguard_device*` as garbage pointer → crash.

**Fix:** Temporarily NULL `netif->state` before calling `netif_set_up`,
`netif_set_link_up`, or `netif_set_link_down`, then restore it immediately after.
The callback receives NULL and bails out cleanly. Applied to 5 call sites:

- `ml_wg_mgr.c` — `wg_init_interface()`: wraps `netif_set_up` + `netif_set_link_up`
- `wireguardif.c` line ~394 — `netif_set_link_up` on handshake complete (session established)
- `wireguardif.c` line ~485 — `netif_set_link_up` in periodic keepalive path
- `wireguardif.c` line ~1158 — `netif_set_link_down` in `wireguardif_network_rx`
- `wireguardif.c` line ~1208 — `netif_set_link_down` in `wireguardif_periodic`

Pattern applied at each site:
```c
device->netif->state = NULL;
netif_set_link_up(device->netif);   /* or set_link_down / set_up */
device->netif->state = device;
```

### 9. ml_config_httpd.c — strncpy → snprintf (CONFIG_ML_ENABLE_CONFIG_HTTPD)
GCC 15 promotes `-Wstringop-truncation` to error.
Three call sites in `ml_config_httpd.c` used `strncpy(dst, src, N)` where `N == strlen_max(src)`,
triggering 6 errors when `CONFIG_ML_ENABLE_CONFIG_HTTPD=y`.
Replaced with `snprintf(dst, sizeof(dst), "%s", src)` — null-safe, no truncation warning.

Affected functions: `config_load_wifi_list` (line ~240), `handler_post_wifi` (line ~878),
`ml_config_get_wifi_list` (line ~937).

## API Additions

### microlink_has_stored_credentials()

```c
bool microlink_has_stored_credentials(void);
```

Returns `true` if a machine key is present in NVS (device previously registered with Tailscale).

**Why:** Allows callers to distinguish between two "no auth key" scenarios:
- No auth key + no NVS session → skip init entirely (avoid futile registration attempt)
- No auth key + NVS session exists → proceed with init (reconnects using cached keys)

**Usage:**

```c
if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] == '\0' && !microlink_has_stored_credentials()) {
    // No key, no session — skip MicroLink
} else {
    // Auth key present OR cached session exists — safe to call microlink_init()
}
```

Must be called before `microlink_init()`. Read-only NVS access (namespace `"microlink"`, key `"machine_pri"`).

## Maintenance — Syncing Upstream Updates

Use `update.sh` in the repo root:

```bash
cd /path/to/microlink-fork
./update.sh
```

The script:
1. Fetches `upstream/main`
2. Shows new commits
3. Prompts confirmation
4. Rebases `esp-idf-6x-compat` onto `upstream/main`
5. Pushes with `--force-with-lease`

If rebase conflicts occur (upstream changed a patched file):
```bash
# Resolve conflicts manually, then:
git rebase --continue
git push -f origin esp-idf-6x-compat
```

## Integration into a Project (vendor/ submodule pattern)

```bash
cd your-project
mkdir -p vendor
git submodule add -b esp-idf-6x-compat https://github.com/fudio101/microlink.git vendor/microlink
ln -s ../vendor/microlink/components/microlink   components/microlink
ln -s ../vendor/microlink/components/wireguard_lwip components/wireguard_lwip
git add .gitmodules vendor/microlink components/microlink components/wireguard_lwip
```

In `platformio.ini`, add credentials overlay (git-ignored):
```ini
board_build.esp-idf.sdkconfig_extra = sdkconfig.credentials
```

`sdkconfig.credentials` (git-ignored):
```
CONFIG_ML_TAILSCALE_AUTH_KEY="tskey-auth-xxxxxxxxxxxx"
CONFIG_ML_DEVICE_NAME="my-device"
```

Wire into WiFi connected event (`app_handlers.c` or equivalent):
```c
#include "microlink.h"
static microlink_t *s_ml = NULL;

// In WIFI_CONNECTED handler:
if (s_ml == NULL) {
    if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] != '\0') {
        const char *dev_name = CONFIG_ML_DEVICE_NAME[0] ? CONFIG_ML_DEVICE_NAME
                                                         : microlink_default_device_name();
        microlink_config_t cfg = {
            .auth_key    = CONFIG_ML_TAILSCALE_AUTH_KEY,
            .device_name = dev_name,
            .enable_derp = true, .enable_stun = true, .enable_disco = true,
            .max_peers   = CONFIG_ML_MAX_PEERS,
        };
        s_ml = microlink_init(&cfg);
        if (s_ml) microlink_start(s_ml);
    }
} else {
    microlink_rebind(s_ml);  // WiFi reconnected — reopen sockets, keep VPN session
}
```
