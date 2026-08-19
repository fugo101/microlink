# MicroLink

Tailscale-protocol network stack for ESP32 — ts2021 control plane, WireGuard
data plane, DISCO/STUN path discovery, DERP relay, and optional 4G cellular
with WiFi failover. FuGo fork of [CamM2325/microlink](https://github.com/CamM2325/microlink),
ESP-IDF 6.x only.

Published to the ESP Component Registry as `fugo101/microlink`. Depends on
[fugo101/wireguard_lwip](https://components.espressif.com/components/fugo101/wireguard_lwip)
for the WireGuard data plane.

## Install

```bash
idf.py add-dependency "fugo101/microlink^3.0.0"
```

## Requirements

- ESP-IDF 6.0.x (5.x is not supported by this fork — `src/ml_noise.c` needs
  `<mbedtls/private/chachapoly.h>`, only present in the TF-PSA-Crypto tree
  shipped from ESP-IDF 6.0 onward)
- ESP32 with WiFi (ESP32-S3 with PSRAM recommended for large tailnets)
- Tailscale account with a (reusable) auth key

## Documentation

Full README, architecture overview, build/flash instructions, examples, and
`CHANGELOG.md` live in the project repository:
https://github.com/fugo101/microlink

## License

MIT — see [LICENSE](https://github.com/fugo101/microlink/blob/main/LICENSE).
