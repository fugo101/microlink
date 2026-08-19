# Changelog

## [3.1.1](https://github.com/fugo101/microlink/compare/v3.1.0...v3.1.1) (2026-08-19)


### Bug Fixes

* **kconfig:** make ML_CONFIG_HTTPD properly depend on ML_ENABLE_CONFIG_HTTPD ([#71](https://github.com/fugo101/microlink/issues/71)) ([f9d8630](https://github.com/fugo101/microlink/commit/f9d86303d36036b789aad00bda4476631cd1c95b))


### Documentation

* configure agent skills (issue tracker + domain docs) ([#70](https://github.com/fugo101/microlink/issues/70)) ([1c502b6](https://github.com/fugo101/microlink/commit/1c502b67fdacf74061a27c6249b7e9130999b21f))
* **microlink:** add component-root README for registry publish ([#68](https://github.com/fugo101/microlink/issues/68)) ([29a0af2](https://github.com/fugo101/microlink/commit/29a0af2dc98a5224cfcd310aeef1af7272558233))

## [3.1.0](https://github.com/fugo101/microlink/compare/v3.0.0...v3.1.0) (2026-08-19)


### Features

* **config_httpd:** add CONFIG_ML_CONFIG_HTTPD toggle, fix peer-update queue depth ([#51](https://github.com/fugo101/microlink/issues/51)) ([5475faa](https://github.com/fugo101/microlink/commit/5475faa23d386833d3f404c2bcc802949df815fc))
* **wg:** tailnet-range fallback routing via a designated peer ([#65](https://github.com/fugo101/microlink/issues/65)) ([f3c2128](https://github.com/fugo101/microlink/commit/f3c2128f9a3c29c3aa3b0f506cbb5d1e519a2ed0))


### Bug Fixes

* **config_httpd:** gate temp-sensor code behind CONFIG_SOC_TEMP_SENSOR_SUPPORTED ([#50](https://github.com/fugo101/microlink/issues/50)) ([8e49f6b](https://github.com/fugo101/microlink/commit/8e49f6bbab0d4e63a0b237a8b4fab9abf5c500cd))
* **coord,wg:** authoritative peer sweep + NVS cache removal on peer removal ([#63](https://github.com/fugo101/microlink/issues/63)) ([2694ec0](https://github.com/fugo101/microlink/commit/2694ec0a98531503b3c865bdaf71332cfc90f219))
* **coord:** back off minutes behind a captive portal instead of every 16s ([#48](https://github.com/fugo101/microlink/issues/48)) ([c77ab55](https://github.com/fugo101/microlink/commit/c77ab55a9d4cac4e28da5bf92ba3ffd99f5caa3a))
* **coord:** stream-liveness watchdog for silently-dead mapSessions ([#58](https://github.com/fugo101/microlink/issues/58)) ([00f168d](https://github.com/fugo101/microlink/commit/00f168d7466b3c1e61cc9933a3eeb6aaaa89c1ca))
* **derp,coord:** absorb bugfix/hardening subset of upstream PR [#22](https://github.com/fugo101/microlink/issues/22) ([#44](https://github.com/fugo101/microlink/issues/44)) ([2aedd0a](https://github.com/fugo101/microlink/commit/2aedd0a5d29a749f2eb11e713d1cba3847a2619c))
* **derp:** backpressure retry budget, TCP_NODELAY, coalesced writes, hot-spin loop fix ([#62](https://github.com/fugo101/microlink/issues/62)) ([1757ef4](https://github.com/fugo101/microlink/commit/1757ef4e1fd228b0a6355223e5f2668696e5e890))
* **derp:** DERP TLS session resumption, longer connect timeout, upgrade backoff ([#52](https://github.com/fugo101/microlink/issues/52)) ([e48d8c1](https://github.com/fugo101/microlink/commit/e48d8c1796ec6376da3a9bd8775014d51516aa50))
* **microlink:** worker task liveness bitmask, orphan-parked teardown ([#47](https://github.com/fugo101/microlink/issues/47)) ([6a7d8c5](https://github.com/fugo101/microlink/commit/6a7d8c52cf9b969988bbea67db4aeaea0e3ff795))
* **wg_mgr:** retry the initial direct-path handshake instead of one-shot ([#54](https://github.com/fugo101/microlink/issues/54)) ([7e52ba3](https://github.com/fugo101/microlink/commit/7e52ba37b850cf3dd07fb89f50a09deca5d19303))
* **wg_mgr:** stop DISCO PING starvation from collapsing throughput to DERP ([#55](https://github.com/fugo101/microlink/issues/55)) ([e46aa5e](https://github.com/fugo101/microlink/commit/e46aa5eda10b3efc386a87e778cc936af3f11079))
* **wg_mgr:** wg_udp_output_cb thread safety, standard MTU, handshake gating bug ([#53](https://github.com/fugo101/microlink/issues/53)) ([47b1ed0](https://github.com/fugo101/microlink/commit/47b1ed06b44358e07713b3d123c7116d2dd39a6b))
* **wg_mgr:** yield 1 tick per peer in disco_periodic_probes ([#49](https://github.com/fugo101/microlink/issues/49)) ([dde0925](https://github.com/fugo101/microlink/commit/dde092569b473074fcc15012a5e1eebae90bf639))
* **wg:** rate-limit DISCO PONG replies once a direct path is trusted ([#64](https://github.com/fugo101/microlink/issues/64)) ([ac9e156](https://github.com/fugo101/microlink/commit/ac9e156de30ba428f825d052e4bafa2318060355))


### Documentation

* **fork-prs:** [#34](https://github.com/fugo101/microlink/issues/34)/[#42](https://github.com/fugo101/microlink/issues/42) landed in wireguard_lwip submodule PR, [#31](https://github.com/fugo101/microlink/issues/31) not needed ([#59](https://github.com/fugo101/microlink/issues/59)) ([05385d8](https://github.com/fugo101/microlink/commit/05385d8973f218e2dd6f322cae01d1db67844855))
* **fork-prs:** close [#35](https://github.com/fugo101/microlink/issues/35) — reader/writer split decided not to port ([#66](https://github.com/fugo101/microlink/issues/66)) ([78e70d6](https://github.com/fugo101/microlink/commit/78e70d683a679ee4a18a03865339644d79d75dc8))
* **fork-prs:** mark [#31](https://github.com/fugo101/microlink/issues/31), [#36](https://github.com/fugo101/microlink/issues/36), [#41](https://github.com/fugo101/microlink/issues/41) as decided (no port needed) ([#61](https://github.com/fugo101/microlink/issues/61)) ([48df8f5](https://github.com/fugo101/microlink/commit/48df8f5bfb29871e1d970273f804c580e717bde7))
* **fork-prs:** record investigation findings for issues [#35](https://github.com/fugo101/microlink/issues/35), [#38](https://github.com/fugo101/microlink/issues/38)/[#39](https://github.com/fugo101/microlink/issues/39), [#41](https://github.com/fugo101/microlink/issues/41) ([#56](https://github.com/fugo101/microlink/issues/56)) ([b3260c7](https://github.com/fugo101/microlink/commit/b3260c72f287230339516e510c248c968baa92f6))
* **fork-prs:** resolve issue [#30](https://github.com/fugo101/microlink/issues/30) by verification, no code change needed ([#57](https://github.com/fugo101/microlink/issues/57)) ([465698f](https://github.com/fugo101/microlink/commit/465698f9fd6ba890c28e5fc7c7aad43c9a266d2d))

## [3.0.0](https://github.com/fugo101/microlink/compare/v2.1.0...v3.0.0) (2026-08-14)


### ⚠ BREAKING CHANGES

* ESP-IDF 5.x is no longer supported; 6.0.x is required.

### Features

* add release-please, formalize ESP-IDF 6.x-only as a breaking change ([44c3dd4](https://github.com/fugo101/microlink/commit/44c3dd4ee71c412341c40ce7478aa5233fa1dc60))


### Bug Fixes

* declare esp_driver_uart/esp_driver_gpio for the cellular build ([#9](https://github.com/fugo101/microlink/issues/9)) ([ea9237a](https://github.com/fugo101/microlink/commit/ea9237a8b0f0ef712ee4a6f8661b44d890d48f18))
