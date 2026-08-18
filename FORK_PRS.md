# Fork mining record

Tracks GitHub forks of [`CamM2325/microlink`](https://github.com/CamM2325/microlink) that were
investigated for fixes/features worth pulling into this fork — distinct from
[`UPSTREAM_PRS.md`](UPSTREAM_PRS.md), which tracks upstream's own six open PRs. Exists so this
research (51 forks surveyed, 13 with real divergence, 4 parallel investigations) doesn't have to be
redone, and so a future contributor knows which fork commits map to which tracking issue.

**Survey status as of 2026-08-18:** 51 forks total. 38 are pure stale mirrors (identical `pushed_at`
to upstream's last commit, `216da33`, 2026-03-17) or otherwise trivially behind — not investigated
individually. 13 forks have real commits ahead of upstream; all 13 were inspected directly (commit
diffs, not just messages).

## Worth pulling — tracked as issues

Each row is one independently-shippable fix/feature, tracked as its own GitHub issue so it can
become its own scoped PR. Issue numbers filled in once created.

| # | Issue | Source fork | Commit(s) | Summary | Tier |
|---|-------|-------------|-----------|---------|------|
| 1 | ✅ [#14](https://github.com/fugo101/microlink/issues/14) (done) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `38602ab0`, `b25b1eee` | DERP TLS context leak on every failed `ml_derp_connect()` (~3-8KB/attempt, verified on hardware). Pre-existing issue filed 2026-08-17 from `UPSTREAM_PRS.md`'s "mineable from #22" note; enriched here with the cplewes source instead of building `derp_tls_abort()` from scratch. Adapted (not literal cherry-pick, our fork lacks entropy/ctr_drbg fields) into `ml_derp.c`'s `derp_free_tls_state()` + `fail_tls` goto path. | 1 |
| 2 | ✅ [#21](https://github.com/fugo101/microlink/issues/21) (done) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `a415d646` | Teardown UAF: `microlink_stop()`/`destroy()` didn't join worker tasks before freeing context; replaces "sleep 3s and hope" with a real per-task liveness bitmask + reap-orphans path | 1 |
| 3 | ✅ [#22](https://github.com/fugo101/microlink/issues/22) (done) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `7120dfa4` | Three crash sites triggered by captive-portal DNS failures: DERP double-init dangling pointer, `ml_coord_task` touching a freed event group post-destroy, corrupt-state races during parallel teardown. Landed together with #21 in the same PR — `a415d646` (the commit for #21) supersedes most of `7120dfa4`'s NULL-guard approach with a proper liveness bitmask; only the DERP mbedTLS double-init fix from `7120dfa4` was a distinct, still-needed piece. | 1 |
| 4 | [#23](https://github.com/fugo101/microlink/issues/23) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `8f2ff39b`, `5c7303b4` | Captive-portal detection (HTTP 302 / TCP close) in `ml_coord.c`, backs off 5 min instead of retrying every 16s | 1 |
| 5 | ✅ [#24](https://github.com/fugo101/microlink/issues/24) (done) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `9f6af750` | Yield 1 tick per peer in `disco_periodic_probes` — prevents starving other same-core tasks on large tailnets | 1 |
| 6 | [#25](https://github.com/fugo101/microlink/issues/25) | [`cplewes/microlink`](https://github.com/cplewes/microlink) | `b9636816` | DERP TLS session resumption (skip full ECDHE handshake on reconnect, ~7.5s→sub-second), connect timeout 10s→25s, per-peer exponential backoff on direct-path upgrade probes | 1 |
| 7 | [#26](https://github.com/fugo101/microlink/issues/26) | [`antmanler/microlink`](https://github.com/antmanler/microlink) (branch `duoduo-edge`) | `2a7ba328` | `wg_udp_output_cb` thread-safety bug (raw UDP PCB called from two task contexts, corrupts heap) — same bug class as absorbed upstream PR #20, opposite direction; bundled with WG/DISCO RX queue depth increase (8→32/16) and a handshake-gating bug in `process_disco_pong` that permanently blocked sessions | 1 |
| 8 | ✅ [#27](https://github.com/fugo101/microlink/issues/27) (done) | [`antmanler/microlink`](https://github.com/antmanler/microlink) | `6ef9f5a0` | New `CONFIG_ML_CONFIG_HTTPD` Kconfig toggle to skip the port-80 httpd task (~7-8KB RAM saved); peer-update queue depth fix 400→32 (was allowing an unbounded ~80KB internal-RAM burst). Not a literal cherry-pick — `git apply` failed on unrelated line drift from PR #47's teardown rewrite, so hand-ported with identical logic/comments. | 1 |
| 9 | [#28](https://github.com/fugo101/microlink/issues/28) | [`antmanler/microlink`](https://github.com/antmanler/microlink) | `2a7ba328` | WG netif MTU 1420→1280 (Tailscale-standard) — needs verification against our existing `CONFIG_LWIP_IP4_FRAG`/`IP4_REASSEMBLY=y` before deciding it's still needed | 1 |
| 10 | ✅ [#29](https://github.com/fugo101/microlink/issues/29) (done) | [`liestrela/microlink`](https://github.com/liestrela/microlink) | `1649d987` | Gate ESP32 temp-sensor feature behind `CONFIG_SOC_TEMP_SENSOR_SUPPORTED` — `esp_driver_tsens` was an unconditional `REQUIRES`, breaks build on SoC variants without a temp sensor. Cherry-picked verbatim, no adaptation needed. | 1 |
| 11 | [#30](https://github.com/fugo101/microlink/issues/30) | `cplewes` vs `Csontikka` (conflict) | `cplewes` `83a102be`/`01d51697` vs [`Csontikka/microlink`](https://github.com/Csontikka/microlink) `dd5714c4`→`2e68e546` | `ip_input()` vs `netif->input`/`tcpip_input` threading fix for decrypted WG RX pbufs — cplewes fixes a UAF this way, Csontikka's own branch later reverted the equivalent fix citing a throughput regression (~30pps cap). Needs research, not a blind cherry-pick | 2 |
| 12 | [#31](https://github.com/fugo101/microlink/issues/31) | `Csontikka/microlink` | `647c2ab0` | `LOCK_TCPIP_CORE` around `netif_set_link_up/down` — compare against our documented `netif->state` NULL-and-restore workaround in `ESP_IDF_6X_COMPAT.md` before adopting | 2 |
| 13 | [#32](https://github.com/fugo101/microlink/issues/32) | `Csontikka/microlink` | `5bda1783` | Peer direct-handshake init was one-shot; peers with dropped/unanswered handshakes stayed permanently blackholed — retry every 30s via timestamp instead of boolean latch | 2 |
| 14 | [#33](https://github.com/fugo101/microlink/issues/33) | `Csontikka/microlink` | `2e68e546`, `f1de3143` | DISCO trust-expiry gate (15s→60s, checks `last_rx` first), NAT-rebind handshake-skip when data path still alive, SPIRAM pbuf headroom fix for `wg_udp_output_cb` | 2 |
| 15 | [#34](https://github.com/fugo101/microlink/issues/34) | `Csontikka/microlink` | `b7442d0f` | Handshake retries throttled to 1/tick round-robin instead of firing to every eligible peer in one tick (was blocking the caller ~280ms with 7 peers) | 2 |
| 16 | [#35](https://github.com/fugo101/microlink/issues/35) | `Csontikka/microlink` | `7a24a9b3`, `4d3b3add` | DERP client backpressure/reconnect-storm fixes: don't tear down on TLS write backpressure, TCP_NODELAY + coalesced writes, fix a hot-spinning I/O loop, split into concurrent reader/writer tasks — check overlap against cplewes's DERP session-resumption fix (#6) before scoping | 2 |
| 17 | [#36](https://github.com/fugo101/microlink/issues/36) | `Csontikka/microlink` | `27806be3` | `microlink_stop()` never closed `derp.sockfd`/`coord_sock`, letting `derp_tx` outlive the teardown wait → UAF — reconcile against cplewes's teardown UAF fix (#2), may be the same root cause | 2 |
| 18 | [#37](https://github.com/fugo101/microlink/issues/37) | `Csontikka/microlink` | `cbdf1603`, `aad403af`, `533f1f88`, `46e34917`, `017b3588`, `372ca277` | H2 frame reassembly across `noise_recv()` read boundaries — large MapResponses spanning reads deterministically corrupt the stream ("implausible message size"); genuine protocol-correctness bug independent of control-plane backend | 2 |
| 19 | [#38](https://github.com/fugo101/microlink/issues/38), [#39](https://github.com/fugo101/microlink/issues/39) | [`djorr5/microlink`](https://github.com/djorr5/microlink) | `67b230b2` | Two independent changes, split into two issues: (a) dynamic H2 RX window sizing based on free heap for RAM-constrained boards, roughly halving MapResponse-parsing PSRAM footprint; (b) `ip4_route_src_hook` to force tailnet-range traffic onto the WG netif directly | 2 |
| 20 | [#40](https://github.com/fugo101/microlink/issues/40) | [`AELovelace/LAIN-MicrolinkRouter`](https://github.com/AELovelace/LAIN-MicrolinkRouter) | `e1239460` | `microlink_set_exit_node()` — default-route exit-node support while preserving per-peer `/32` routes, with routing-loop and liveness fixes. New capability, not a bugfix — needs a product decision (does any current/future downstream want SoftAP/NAPT exit-node routing?) before it's worth the review cost | 2 |
| 21 | [#41](https://github.com/fugo101/microlink/issues/41) | [`caslavskola/microlink`](https://github.com/caslavskola/microlink) | `0265718e` | PSA crypto init migration for mbedTLS 4.x — verify overlap with existing `ESP_IDF_6X_COMPAT.md` PSA work; may be partially/fully redundant | 2 |
| 22 | [#42](https://github.com/fugo101/microlink/issues/42) | `caslavskola/microlink` | `9ac49212` | Peer endpoint tracking dropped the "packet arrived via DERP" signal after the first packet, causing replies to attempt bad direct routing — genuine bug, but the commit bundles debug cruft and a divergent netif-flag rewrite; extract just the DERP-routing-flag fix | 2 |
| 23 | [#43](https://github.com/fugo101/microlink/issues/43) | [`dj-oyu/microlink`](https://github.com/dj-oyu/microlink) | multiple (different/pre-refactor file naming) | Bundle of fixes needing adaptation to our `ml_*.c` layout: ~~TAI64N via `gettimeofday()` (fixes replay-rejection after reboot)~~ **superseded — see `UPSTREAM_PRS.md`'s upstream PR #22 (partial) absorption, commit `6a4447c5`: cleaner source, no pre-refactor adaptation needed, don't redo this from dj-oyu**; DERP→direct re-handshake trilogy (`a30160a8`, `eb3dce06`, `c505539d`), `LOCK_TCPIP_CORE` no-op bug fixed via `tcpip_try_callback` (check `ml_net_io.c`/`ml_zerocopy.c` exposure), DISCO PONG rate-limiting (1/5s per peer once direct path established) | 2 |

## Investigated, not worth pursuing

| Fork | Why skipped |
|---|---|
| `lixy123/microlink`, `szf2020/microlink`, `CryptoKylan/microlink` | Pure stale mirrors of upstream, zero meaningful divergence. |
| `flowjob1/microlink` (branch `combined`) | Board-specific Waveshare 4G-USB-modem hardware enablement (out of scope) plus a `json`→`cjson` fix we already have (`ESP_IDF_6X_COMPAT.md`). Messy WIP history. |
| `chat-l18l/microlink` | Exploratory/unfinished streaming-MapResponse rewrite (commit trail reads "checkpoint", no verification) — Csontikka's own equivalent work in the same area is more mature and battle-tested; prefer that lineage. A WiFi-optional/ESP32-P4 portability commit is mildly interesting but not for a target we support. |
| `sumagnadas/microlink`, `LuccaMS/microlink` | Personal app-layer product work (homelab dashboard, Wake-on-LAN example) on top of `ml_config_httpd`, not core-library fixes. |
| `cadl/libts3ds` | Full Nintendo 3DS rebrand/port with its own backend abstraction — not general-purpose fixes. |
| `youngthuggayslxo-code/microlink` | Single commit adds a nonsensical MSBuild CI workflow to a CMake/ESP-IDF project. Junk. |
| `GrieferPig/microlink` (1 star) | Real fix (peer-lookup keypair preference + src/dest IP validation per WireGuard spec) — but verified already present and correct in our `components/wireguard_lwip` submodule. No action needed. |

## Notes

- None of the 13 diverged forks are owned by the three already-absorbed upstream PR authors
  (snowpaper, letalvoj, pepabo/kentaro) — confirmed independent work, no double-absorption risk.
- `cplewes/microlink` and `Csontikka/microlink` both use our post-refactor `ml_*.c` file naming,
  making their commits directly comparable/cherry-pickable. `dj-oyu/microlink` still uses upstream's
  pre-refactor naming (`microlink_wireguard.c` etc.) — every fix from it needs re-implementation
  against our layout, not a cherry-pick.
- Two explicit conflicts to resolve before scoping PRs: (1) the `ip_input()`/`netif->input` fix
  (#11) where `cplewes` and `Csontikka`'s own history disagree; (2) potential overlap between
  `cplewes`'s teardown UAF fix (#2) and `Csontikka`'s socket-shutdown-on-stop fix (#17) — likely the
  same root cause approached from different angles, verify before duplicating work.
