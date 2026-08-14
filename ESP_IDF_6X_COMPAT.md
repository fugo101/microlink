# ESP-IDF 6.x Compatibility

`esp-idf-6x-compat` was fast-forward merged into `main` on 2026-08-12 and is no longer a separate
branch -- these patches live on `main`.

Upstream MicroLink targets ESP-IDF v5.x. This fork carries patches to build cleanly on
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

`update.sh` (which rebased a branch that no longer exists, `esp-idf-6x-compat`, onto
`upstream/main`) has been removed -- see [UPSTREAM_PRS.md](UPSTREAM_PRS.md)'s "known-stale" note
for why it stopped being safe to run. In its place:

- `.github/workflows/upstream-drift.yml` runs weekly (and on demand via `workflow_dispatch`) and
  opens/updates a single tracking issue (label `upstream-drift`) listing any upstream commits or
  open PRs not yet accounted for.
- Absorb a specific upstream PR with `git fetch upstream refs/pull/N/head && git cherry-pick -x
  <sha>` (preserves the original author, we appear only as committer), record it in
  [UPSTREAM_PRS.md](UPSTREAM_PRS.md), and open a PR whose title is a valid Conventional Commit
  (e.g. `fix(wireguard_lwip): ... (upstream #N)`) -- the cherry-pick itself keeps upstream's
  message; only the squash-merge title needs to be Conventional.

## Integration into a Project

Via the ESP Component Registry:

```yaml
# your_project/main/idf_component.yml
dependencies:
  fugo101/microlink:
    version: "^3.0.0"
```

`fugo101/wireguard_lwip` comes along transitively -- never declare it yourself. See the README's
Installation section for the `override_path` recipe used when developing against a local checkout
of this repo instead of a published version.

**Credentials.** `board_build.esp-idf.sdkconfig_extra` is **not a real PlatformIO option** — it is
silently ignored, so a key placed in a separate "overlay" file this way is never applied. PlatformIO
only reads `board_build.esp-idf.sdkconfig_path`, and that option *replaces* the sdkconfig outright
rather than layering onto it (verified against `espidf.py`, which passes it as `-DSDKCONFIG=<path>`).

If your project's sdkconfig is already gitignored (the default for a fresh PlatformIO project),
there is nothing to work around — set the credentials via `pio run -t menuconfig` or edit the
tracked sdkconfig directly; it never reaches git.

If your project intentionally tracks its sdkconfig (as some do, to keep the full build config
reviewable, with a separate guard against committing secrets), point at a **complete**, untracked
sdkconfig instead — not a fragment:
```ini
; platformio.ini — local-only, keep out of git
board_build.esp-idf.sdkconfig_path = sdkconfig.local
```
```bash
cp sdkconfig.<env-name> sdkconfig.local   # then set the credentials in sdkconfig.local
echo sdkconfig.local >> .gitignore
```

Either way, the values you need:
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
