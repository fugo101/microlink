# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# Source ESP-IDF first (required every session)
source ~/esp/esp-idf/export.sh

# Build an example
cd examples/basic_connect   # or: cellular_connect, cellular_heartbeat, failover_connect, rebind_test
idf.py build

# Flash + monitor
idf.py -p /dev/ttyACM0 flash monitor

# Clean build (required after pin/config changes)
idf.py fullclean && idf.py build

# Interactive config
idf.py menuconfig   # → MicroLink V2 Configuration
```

## Credentials Setup

Credentials are **never in source** — always in `sdkconfig` (gitignored):

```bash
cd examples/<example_name>
cp sdkconfig.credentials.example sdkconfig.credentials
# Edit with WiFi SSID/password, Tailscale auth key, cellular APN, etc.
```

Or set via `idf.py menuconfig` → MicroLink V2 → Credentials.

## Architecture

Five FreeRTOS tasks communicating via queues — no shared mutable state, no mutexes between tasks:

| Task | Core | Priority | Stack | Role |
|------|------|----------|-------|------|
| `net_io` | 0 | 7 | 8KB | `select()` loop — unified socket dispatch |
| `derp_tx` | 0 | 5 | 14KB | Sole TLS writer for DERP relay |
| `coord` | 1 | 5 | 12KB | Control plane: Noise handshake, HTTP/2, registration |
| `wg_mgr` | 1 | 7 | 8KB | WireGuard + DISCO + STUN + peer management |

**IPC**: Queues only. Each task owns its data exclusively. No blocking cross-task calls.

### Source Map

```
components/microlink/
  include/
    microlink.h           — Public API (opaque handles, all user-facing functions)
    microlink_internal.h  — Internal types, queue depths, task handles, constants
    ml_cellular.h         — Cellular modem (PPP + AT socket bridge)
    ml_net_switch.h       — WiFi/cellular failover state machine
    ml_config_httpd.h     — HTTP config server (ifdef-gated)
  src/
    microlink.c           — Init, start, stop, destroy, callbacks, rebind
    ml_coord.c            — Tailscale control plane (ts2021, HTTP/2, MapResponse)
    ml_derp.c             — DERP relay client (TLS, region discovery)
    ml_net_io.c           — select() loop, packet dispatch to WG/DISCO/STUN queues
    ml_wg_mgr.c           — WireGuard peer management, DISCO, STUN
    ml_noise.c            — Noise IK handshake (Tailscale's variant)
    ml_h2.c               — HTTP/2 implementation (for control plane)
    ml_udp.c / ml_tcp.c   — User-facing UDP/TCP socket API over WG tunnel
    ml_peer_nvs.c         — NVS peer cache (fast DISCO on reboot)
    ml_cellular.c         — PPP data path + AT socket bridge fallback
    ml_at_socket.c        — AT command socket bridge (slow path)
    ml_net_switch.c       — WiFi/cellular switching, health monitoring, rebind
    ml_config_httpd.c     — Web UI + REST API
    ml_zerocopy.c         — Zero-copy WG receive via raw lwIP PCB
    nacl_box.c / x25519.c — Crypto primitives
  components/wireguard_lwip/
    src/wireguard.c       — WireGuard protocol state machine
    src/wireguardif.c     — lwIP netif integration
    src/crypto/           — ChaCha20-Poly1305, BLAKE2s, X25519
```

### Key Design Decisions

**`microlink_rebind()`**: Switches network interface without destroying the VPN session. Closes/reopens all sockets, signals coord+DERP to reconnect. Preserves WG peer state, crypto keys, VPN IP (~330ms rebind, ~7s full recovery). Called automatically by `ml_net_switch` during WiFi/cellular transitions.

**Cellular data path**: PPP is strongly preferred — gives real lwIP sockets, direct UDP, NAT traversal. AT socket bridge is automatic fallback when PPP auth fails. PPP throughput ~6.5 KB/s vs ~0.45 KB/s for AT bridge.

**PSRAM usage**: H2 receive buffer (512KB) and JSON parse buffer (512KB) are allocated from PSRAM during coordination, then freed. Without PSRAM, reduce via `CONFIG_ML_H2_BUFFER_SIZE_KB=64` (supports ~30 peers max).

**NVS namespaces**: `"microlink"` for keys (machine key, WG key, DISCO key), `"ml_peers"` for peer cache. `microlink_factory_reset()` erases both — must call before `microlink_init()`.

## Required sdkconfig Settings

```ini
# PSRAM (ESP32-S3 with PSRAM)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768

# TLS (required for DERP + control plane)
CONFIG_ESP_TLS_USING_MBEDTLS=y
CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y

# Networking
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_IP4_FRAG=y
CONFIG_LWIP_IP4_REASSEMBLY=y

# Stack
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

## Testing

No automated unit tests — all testing is hardware-in-the-loop. See `TESTING_GUIDE.md` for full procedures.

```bash
# From a PC on the same Tailscale network:
tailscale ping esp32-microlink          # DISCO layer test
ping <ESP32_VPN_IP>                     # ICMP through WG tunnel
echo "ping" | nc -u <ESP32_VPN_IP> 9000 # UDP echo test
```

## Common Pitfalls

- `ML_CELLULAR_PWRKEY_PIN undeclared` after config changes: run `idf.py fullclean`
- Flash fails on XIAO: hold BOOT button while pressing reset before flashing
- `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` does not exist in recent ESP-IDF — use `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` instead
- WiFi power save must be disabled (`WIFI_PS_NONE`) for low-latency WireGuard traffic
- Auth keys must be **reusable** for development (single-use keys expire after first registration)
