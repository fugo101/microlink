# Upstream PR absorption record

Tracks which open PRs against [CamM2325/microlink](https://github.com/CamM2325/microlink) have
been cherry-picked into this fork, and why the rest were skipped. Exists so a future revival of
upstream (or a future `update.sh` run) doesn't double-apply what's already here, and so the
reasoning for skipping the rest doesn't have to be rediscovered.

**Upstream status as of 2026-08-14:** last commit `216da33` on 2026-03-17 (~5 months). Six PRs open,
zero maintainer comments or reviews on any of them. All six are based on exactly `216da33` — the
commit our own compat work (`fce0875` onward) also sits on top of.

## Absorbed

| Upstream PR | Author | Upstream commit | Our commit | Merged | Notes |
|---|---|---|---|---|---|
| [#21](https://github.com/CamM2325/microlink/pull/21) | snowpaper | `da55beb7` | `5d8369c` (PR [#4](https://github.com/fudio101/microlink/pull/4)) | 2026-08-14 | Verbatim cherry-pick. Fixes [#18](https://github.com/CamM2325/microlink/issues/18) — CGNAT peers got a one-way tunnel because an unvalidated advertised endpoint was installed at peer-add. |
| [#20](https://github.com/CamM2325/microlink/pull/20) | snowpaper | `e74be464` | `1314bab` (PR [#5](https://github.com/fudio101/microlink/pull/5)) | 2026-08-14 | Verbatim cherry-pick + a docs commit (`be5c0e3`) updating `components/wireguard_lwip/README.md`'s divergence record. Fixes [#17](https://github.com/CamM2325/microlink/issues/17) — RX path called `ip_input()` directly instead of via `netif->input`, racing `tcpip_thread`. |
| [#23](https://github.com/CamM2325/microlink/pull/23) | letalvoj | `4cc3dded` | `82e5518` (PR [#6](https://github.com/fudio101/microlink/pull/6)) | 2026-08-14 | Verbatim cherry-pick. Logs `RegisterResponse.Error` instead of failing silently into a misleading downstream "node not found". |

All three cherry-picked via `git fetch upstream refs/pull/N/head` + `git cherry-pick -x`, which
preserves the original author; we appear only as committer. Each carries an `Upstream-PR:` /
`Upstream-Issue:` trailer.

## Deliberately skipped (for now)

| Upstream PR | Why skipped | Revisit when |
|---|---|---|
| [#24](https://github.com/CamM2325/microlink/pull/24) — peer `online` from real `Node.Online` | The only one of the four small PRs that conflicts against this fork (one line at `ml_wg_mgr.c` where our `strncpy`→`memcpy` compat change sits on the same hunk). Its entire payload is the `microlink_peer_info_t.online` field delivered via the peer callback — the downstream consumer we checked (ZenClock) never registers a peer callback and never calls `microlink_get_peer_info()`/`get_peer_count()`, so the fix is currently unobservable. Correct fix, just no consumer yet. | The day any downstream project registers a `peer_cb` and reads `.online`. |
| [#22](https://github.com/CamM2325/microlink/pull/22) — custom (non-Tailscale) control planes: TLS control, streaming map-poll, DERP | +3424/−2671, effectively a rewrite of `ml_coord.c`. Built for headscale/Ionscale-style deployments; we run Tailscale's own SaaS control plane, so the TLS-control-transport, streaming-map-poll, and DERP-region-fallback pieces don't apply. Merging it whole against our diverged fork is a rewrite-on-rewrite conflict, not a cherry-pick. | If a downstream project needs a non-Tailscale control plane. |
| ↳ mineable from #22: `derp_tls_abort()` | `ml_derp_connect()`'s failure paths only close the socket, never free the mbedTLS context — leaks ~20 KB of TLS I/O buffers (internal RAM under `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`) on every failed DERP connect attempt, and `microlink_rebind()` reconnects DERP on every WiFi reconnect. This leak fix is independent of the headscale-specific parts of #22 and is worth taking on its own — **not done yet**, needs adapting: our fork already deleted the `entropy`/`ctr_drbg` init+free pair from `ml_derp.c` for mbedTLS 4.x compat (see `ESP_IDF_6X_COMPAT.md`), so `derp_tls_abort()` would need to free only `ssl`/`ssl_conf`, not the fields we removed. | Next microlink maintenance pass — tracked here explicitly so it isn't lost. |
| [#25](https://github.com/CamM2325/microlink/pull/25) — optional TLS for the control-plane transport | Overlaps/superseded in scope by #22 (same non-Tailscale-SaaS use case), and its diff includes a stray committed clangd index binary (`examples/eth_connect/.cache/clangd/index/...`) that shouldn't be merged as-is regardless. | Same trigger as #22. |
| [#14](https://github.com/CamM2325/microlink/pull/14) (closed, not merged) — cross-network web access / thread safety | Closed by upstream without merging; not re-evaluated here. | — |
| [#3](https://github.com/CamM2325/microlink/pull/3) (closed, not merged) — DISCO PONG rate-limit | Author closed it themselves: "Created against wrong repo." | — |

## Double-apply protection

Two layers, since a patch-id compare alone won't catch the adapted subset above:

```bash
# Catches the 3 verbatim cherry-picks (matches by patch content)
git cherry -v upstream/main main

# Catches all of them by commit trailer
git log --grep="cherry picked from"
```

If upstream ever revives and merges #20/#21/#23 themselves, a future `git fetch upstream && git
merge upstream/main` (or a rebase via `update.sh`) will conflict on those hunks — that's expected;
resolve by keeping our side, since it's the same change.

## `update.sh` — known-stale, not fixed here

`update.sh` rebases a branch named `esp-idf-6x-compat` onto `upstream/main` and force-pushes. That
branch is already merged into `main` (`fce0875`) and hasn't moved since; `main` also carries two
more commits (`f377a70`, `2bc586e`) that only exist there, not on `esp-idf-6x-compat`. Running the
script as written would rebase the wrong, stale branch and could ship a `main` missing those two
commits. Left as-is pending a separate fix — don't run it without checking this first.
