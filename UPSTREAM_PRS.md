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
| [#22](https://github.com/CamM2325/microlink/pull/22) (partial) | multiple | `125b5294`, `fcdc8d95`, `ae3d4386`, `8367c1ed`+`9ef10bbb` | PR [#44](https://github.com/fugo101/microlink/pull/44) | in review | **Bugfix/hardening subset only** — adapted, not cherry-picked (base diverged too far). DERP: mbedTLS-context leak on failed connect (already landed as the first commit on this PR, sourced from `cplewes/microlink` — see `FORK_PRS.md` #1/issue #14), region fallback when HomeDERP isn't in the DERPMap, TLS 1.2 pin (Let's Encrypt ECDSA cert / TLS 1.3 OID issue), return-code checking on mbedTLS setup calls, blocking-recv BIO rework. Coord: Hostinfo `OS` field corrected to `"esp32"` (was `"linux"`), new `ML_STATE_AUTH_FAILED` state (registration `Error`/`MachineAuthorized` now actually fails registration instead of silently "succeeding" into a broken state), `state_cb` now fires on the `RECONNECTING` transition and doesn't get clobbered by it while `AUTH_FAILED`. The 3 headscale/custom-control-plane feature commits (`CONFIG_ML_CTRL_HOST`+TLS transport, `ctrl_host`/`ctrl_noise_pubkey`, `streaming_map_fetch`, new `ml_coord_tls.c`) and the already-duplicate `netif->input` fix (`5c8d60c3`, functionally identical to already-absorbed PR #20) are **not** included — see the "Deliberately skipped" row below, now scoped to just those. |

All three verbatim cherry-picks were done via `git fetch upstream refs/pull/N/head` + `git
cherry-pick -x`, which preserves the original author; we appear only as committer. Each carries an
`Upstream-PR:` / `Upstream-Issue:` trailer. PR #22's absorption above is different — the base has
diverged too far for a literal cherry-pick, so it's a hand-adapted equivalent with an `Adapted
from` trailer instead.

## Deliberately skipped (for now)

| Upstream PR | Why skipped | Revisit when |
|---|---|---|
| [#24](https://github.com/CamM2325/microlink/pull/24) — peer `online` from real `Node.Online` | The only one of the four small PRs that conflicts against this fork (one line at `ml_wg_mgr.c` where our `strncpy`→`memcpy` compat change sits on the same hunk). Its entire payload is the `microlink_peer_info_t.online` field delivered via the peer callback — the downstream consumer we checked (ZenClock) never registers a peer callback and never calls `microlink_get_peer_info()`/`get_peer_count()`, so the fix is currently unobservable. Correct fix, just no consumer yet. | The day any downstream project registers a `peer_cb` and reads `.online`. |
| [#22](https://github.com/CamM2325/microlink/pull/22) — custom (non-Tailscale) control planes: TLS control, streaming map-poll | The 3 headscale/Ionscale-specific feature commits only: build-time `CONFIG_ML_CTRL_HOST`+TLS control transport (new `ml_coord_tls.c`), runtime `ctrl_host`/`ctrl_noise_pubkey`, and `streaming_map_fetch` for controllers that ignore `Stream=false`. We run Tailscale's own SaaS control plane, so none of this applies. Everything else in #22 (DERP hardening, Hostinfo OS string, `ML_STATE_AUTH_FAILED`, the reconnect-state-callback fixes) **was absorbed** — see the "Absorbed" table above (PR #22, partial). | If a downstream project needs a non-Tailscale control plane. |
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
merge upstream/main` will conflict on those hunks — that's expected; resolve by keeping our side,
since it's the same change.

## `update.sh` — removed

Used to rebase a branch named `esp-idf-6x-compat` onto `upstream/main` and force-push it. That
branch was already merged into `main` (`fce0875`) and hadn't moved since; `main` also carries two
more commits (`f377a70`, `2bc586e`) that only ever existed there, not on `esp-idf-6x-compat`.
Running the script as written would have rebased the wrong, stale branch and could have shipped a
`main` missing those two commits. Removed rather than fixed — `.github/workflows/upstream-drift.yml`
covers the "is upstream ahead of us" question the script existed for, without the force-push risk.
See [ESP_IDF_6X_COMPAT.md](ESP_IDF_6X_COMPAT.md)'s Maintenance section for the manual cherry-pick
recipe that replaces it.
