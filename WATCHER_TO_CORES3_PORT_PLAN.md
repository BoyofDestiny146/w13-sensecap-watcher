# Watcher to CoreS3 port plan

Phase 1 audit only. No application or board source was changed.

The port in Phases 2–5 is **stopped**. The M5Stack CoreS3 project that contains the compile-tested M144 ES8388 configuration is not in this workspace and is not in any repository this environment can read. Guessing those pins into the stock CoreS3 board would violate the audio-protection rule.

## Sources compared

| Tree | Commit | What it is |
| --- | --- | --- |
| Upstream XiaoZhi base | `236deb2` | Parent of the W-13 commits. Current upstream XiaoZhi at the fork point used by this Watcher repo. |
| Full Watcher behavior | `origin/w13-power` tip `4c9445c` | UI, Nexus Wi-Fi branding, identity preservation, V4 screens. Includes `w13-ui-v1` and `v4-ui-bench`. |
| Checked-out Watcher branch | `w13-sensecap` tip `550eaed` | Only adds the Nexus OTA URL to `sensecap-watcher/config.json`. It does **not** contain the V4 UI. |
| Stock CoreS3 in this repo | same tree as upstream | `main/boards/m5stack/core-s3/`. Onboard AW88298/ES7210. No M144 configuration. |

`git diff 236deb2 origin/w13-power` is 34 files. CoreS3 board files are identical to upstream. `main/mcp_server.cc`, notification players, and audio codecs are not in that diff.

## M144 configuration that must be preserved

This pinout was specified for the CoreS3 + M144 ES8388 module (Configuration B). It is **not present** in git:

- MCLK = GPIO7
- BCLK = GPIO0
- WS = GPIO6
- DOUT = GPIO13
- DIN = GPIO14
- ES8388 I2C address = 0x10

Stock CoreS3 `config.h` in this repo is different and must not be edited to “match” M144 from this audit:

- MCLK = GPIO0 (also `BOOT_BUTTON_GPIO`)
- WS = GPIO33
- BCLK = GPIO34
- DIN = GPIO14
- DOUT = GPIO13
- Codecs: AW88298 and ES7210, not ES8388

Watcher audio (`sensecap-watcher/config.h`) is also different: MCLK GPIO10, BCLK GPIO11, WS GPIO12, DIN GPIO15, DOUT GPIO16. Category F. Do not copy it.

GPIO0 is a hard conflict: stock CoreS3 uses it as I2S MCLK and the boot button; M144 uses it as I2S BCLK. Do not resolve that by editing either map in this task.

## Inventory

Action words: **copy** means the Watcher diff can land unchanged in the CoreS3 tree; **adapt** means the behavior is wanted but the Watcher file is board-shaped; **board-specific** means leave it on Watcher.

| Watcher feature | Category | Watcher files | CoreS3 equivalent | Action | Dependencies | Risk |
| --- | --- | --- | --- | --- | --- | --- |
| Nexus OTA URL default | network/Nexus | `main/Kconfig.projbuild` (`https://ota.nexus.warehouse-13.biz/xiaozhi/ota/`). `w13-sensecap` also sets `CONFIG_OTA_URL` in `main/boards/sensecap-watcher/config.json`. `w13-power` does not set the board json key. | `main/Kconfig.projbuild`. CoreS3 `config.json` has no OTA override. | copy the Kconfig default into the CoreS3 tree. Do not copy the SenseCAP `config.json` flash/partition block. | OTA client in `main/ota.cc` (unchanged). | Global to every board in that firmware tree. Confirm the CoreS3 product should use the Nexus OTA host. |
| Websocket URL fallback | network/Nexus | `main/protocols/websocket_protocol.cc` uses `wss://ws.nexus.warehouse-13.biz/xiaozhi/v1/` when NVS `url` is empty. | Same file. | copy | NVS namespace `websocket`. | A device with an empty URL will no longer use whatever upstream left unset. Token is still not invented. |
| Fixed AP SSID `Nexus-1984` with no MAC suffix | network/Nexus, device identity | `components/esp-wifi-connect/` override (`WifiConfigurationAp::GetSsid`), `main/idf_component.yml` `override_path`, `.gitignore` exception. | Same component path if the CoreS3 tree still depends on `78/esp-wifi-connect` `~3.3.1`. | adapt by vendoring the same override, then diff it against the 3.3.1 tarball so only the SSID line is the W-13 change. | `idf_component.yml`, component manager. | Override can drift from upstream 3.3.1. Do not edit `managed_components/`. |
| DHCP hostname `Nexus-1984` and AP prefix | network/Nexus | `main/boards/common/wifi_board.cc` | Same file. Stock CoreS3 extends `WifiBoard`, so it inherits this. | copy | `esp-wifi-connect` hostname field. | Every `WifiBoard` in that tree gets the Nexus hostname. That is correct for a CoreS3-only Nexus build. |
| Change Wi-Fi identity preservation | shared/application, device identity | `main/application.cc`, `main/application.h` (`suppress_binding_ui_`, NVS `app/cwifi_pend`, preserve `websocket` token, url, and version). | Same files. | copy | NVS `Settings`. UI must honor `ShouldSuppressBindingUi()`. | If the marker is set and the token is missing, the code falls through to normal activation. Do not invent a token. |
| Pre-LVGL black clear | UI, display | `main/display/lcd_display.h`, `main/display/lcd_display.cc`. New `SpiLcdDisplay` argument `clear_color` defaults to `0xFFFF`. | Same files. CoreS3 constructs `SpiLcdDisplay` in `m5stack_core_s3.cc` and can keep the default or pass `0x0000`. | copy the signature (default keeps existing call sites compiling). Pass `0x0000` from CoreS3 only if a black boot is required. | LVGL init order. | Default white preserves other boards. Do not replace the ILI9342 panel driver. |
| V4 screen model: Boot, Provision QR, Waiting for Wi-Fi, Binding, Ready splash, Main, Listening, Speaking | UI | Nested `CustomLcdDisplay` inside `main/boards/sensecap-watcher/sensecap_watcher.cc`. Geometry constants are for a **412×412 circular** panel (`kPerimeterSize = 408`). | `LcdDisplay` / `SpiLcdDisplay` used by `M5StackCoreS3Board`. Panel is **320×240** ILI9342. There is no separate V4 class. | adapt. Extract behavior, then implement a CoreS3 screen flow with new coordinates. Do not copy `sensecap_watcher.cc`. | `SpiLcdDisplay`, fonts already used by LVGL display, `Application` device state. | Highest risk. Coordinates, circular perimeter arcs, and the right-hand wheel menu do not fit 320×240. |
| Nexus provisioning QR | UI, network/Nexus | `main/boards/sensecap-watcher/nexus_wifi_qr.c`, `nexus_wifi_qr.h`, drawn on the provision screen. | No QR asset on CoreS3. Touch currently calls `EnterWifiConfigMode()` only from the starting state. | adapt. Reuse the QR bitmap only if it encodes the same `Nexus-1984` AP. Place it in the 320×240 safe area. | SSID override above. | A Watcher QR that assumes a round layout or a different SSID must be regenerated, not stretched. |
| Binding screen suppressed on Change Wi-Fi | UI, device identity | `ShowBindingScreen` plus `Application::ShouldSuppressBindingUi()`. | CoreS3 uses the stock LVGL status/emotion UI, not these screens. | adapt. After the application copy, the CoreS3 UI must not show a bind code when suppression is set. | `application.cc` change. | Easy to miss because the flag is only consumed by the Watcher display subclass today. |
| Conversation state UI (listening / speaking) | UI | `ShowListeningScreen`, `ShowSpeakingScreen`, `RestoreV4ScreenForDeviceState`. | Stock `LcdDisplay::SetStatus` / emotion / chat message callbacks. | adapt onto the existing display callbacks. Do not add a second LVGL display object beside `SpiLcdDisplay`. | `Application::SetDeviceState`. | Information hierarchy (status, Wi-Fi, battery, talk) can be kept. Circular layout cannot. |
| Volume control during conversation | UI | Watcher knob rotate/click in `sensecap_watcher.cc` (`WheelMode::VolumeAdjust`). | No knob. FT6336 short tap toggles chat. AXP2101/AW9523 are power and LCD reset, not a volume wheel. | adapt only if a touch gesture is specified. Do not map the knob GPIO handler onto the touch ISR. | Audio codec `SetOutputVolume` already exists on `AudioCodec`. | Gesture choice is a product decision. Unspecified here, so do not invent a gesture in the first port. |
| Change Wi-Fi confirmation and `cwifi_pend` | UI, device identity | Wheel menu item `ChangeWifi` in `sensecap_watcher.cc`. Sets NVS `app/cwifi_pend`, calls `SsidManager::Clear()`, reboots. Does not clear UUID, websocket, or `ota_url`. | No menu. `PollTouchpad` has no Change Wi-Fi path. | adapt the NVS contract (already in `application.cc`). The confirm UI must be a new 320×240 screen or an existing settings surface. | `esp-wifi-connect` `SsidManager::Clear`. | Clearing Wi-Fi without the marker causes a full rebind. The marker must be written and verified before reboot, as the Watcher code does. |
| Tasks / reminders placeholder | UI | `ShowTasksPlaceholder` only. No reminder protocol, storage, or scheduler was added. | None. | do not port as a feature. There is no reminder implementation. | none | Building a reminder system would be new product work, not parity. |
| Notifications | UI | `kDeviceStateNotifying` is mapped to the speaking screen. `main/notify/` is unchanged. | Stock state machine and `LcdDisplay` already receive this state. | adapt only the visible status text if the V4 speaking treatment is required. Do not copy Watcher notify hardware. | existing `SetDeviceState`. | Low, if limited to status text. |
| Screen-off listening | UI, display | `CustomLcdDisplay::SetPowerSaveMode` calls `esp_lcd_panel_disp_on_off`, then restores the V4 screen. | `M5StackCoreS3Board::InitializePowerSaveTimer` already calls `GetDisplay()->SetPowerSaveMode`. CoreS3 backlight is `CustomBacklight` on AW9523. | adapt inside CoreS3’s existing power-save path. Keep the ILI9342 driver. If `disp_on_off` fails, use the backlight fallback already in the Watcher code. | panel capability. | Must be tested on a physical CoreS3. Do not assume ILI9342 supports `disp_on_off`. |
| Low-battery style and status row | UI | `SetupV4LowBatteryUi`, `RefreshStatusRow` in the Watcher display subclass. | `LcdDisplay::UpdateStatusBar` and `PowerSaveTimer` / AXP2101 battery path on CoreS3. | adapt status presentation. Do not copy Watcher battery GPIO. | AXP2101 already on CoreS3. | Battery percentage source differs. Do not invent a Watcher fuel-gauge register map. |
| Theme/font rebind after asset refresh | UI | `SetTheme` override rebinds V4 labels so fonts are not left dangling. | Only needed if CoreS3 grows its own long-lived labels. | adapt with the new labels. Not a standalone copy. | assets / theme code, unchanged. | Low if labels are owned by one display object. |
| V4 bench mode | other | `W13_V4_UI_BENCH_MODE` compile flag. Default 0. Knob cycles six screens. | None. | board-specific. Leave it on Watcher. Do not enable it on CoreS3. | knob. | Shipping bench mode would bypass the real state machine. |
| MCP tools | MCP/tool | No diff in `main/mcp_server.cc` or board MCP registration. | Stock CoreS3 MCP from upstream. | nothing to port. | none | Adding tools that were never on Watcher would not be parity. |
| Watcher display, touch, knob, camera, codec, PMIC, expander | display/touch, audio, board-specific | `sensecap_watcher.cc`, `sensecap_audio_codec.*`, `sscma_camera.*`, `config.h` | `m5stack_core_s3.cc`, `cores3_audio_codec.*`, `config.h`, FT6336, ILI9342, AXP2101, AW9523, GC0308 | board-specific. Do not port. | — | Copying these would replace a working CoreS3 bring-up. |
| M144 ES8388 external codec | audio | Not in the Watcher tree. | Expected in the missing CoreS3 project. | board-specific. Do not modify those pins or the I2C address during parity work. | CoreS3 I2C bus and I2S peripheral chosen by that project. | GPIO0 clash with stock CoreS3 MCLK/boot. Stop if the M144 board file is absent. |

## What cannot be copied directly

- The V4 UI lives inside `sensecap_watcher.cc`, not in `lcd_display.*`. Moving that class onto CoreS3 would pull Watcher panel init, knob pins, and 412×412 coordinates with it.
- CoreS3 input is FT6336 tap-to-toggle-chat. Watcher input is a rotary encoder plus click (select, volume, Change Wi-Fi confirm, Talk). Those interactions need a new touch design. This audit does not choose gestures.
- `SpiLcdDisplay`’s new argument is safe only because it has a default. Call sites that pass `0x0000` are a one-line CoreS3 choice, not a driver replacement.
- Shared Nexus settings in `wifi_board.cc`, `Kconfig.projbuild`, and `websocket_protocol.cc` affect every board built from that tree. Apply them in the CoreS3 firmware tree, not by rewriting the SenseCAP board.
- Reminders and extra MCP tools are not in the Watcher diff. Do not create them under the name of parity.
- The M144 pin file is not available here. Do not create one from the pin list in this document. That list is a constraint, not a license to add a new codec driver.

## Proposed order once the CoreS3 + M144 tree is available

Do this on `feature/cores3-watcher-parity` from that tree’s known-good branch. Do not modify its `main`. Compile with `idf.py build` for `esp32s3` / `m5stack-core-s3` after each group. Stop on the first failure. Do not flash.

1. **Shared Nexus configuration.** Kconfig OTA URL, websocket URL fallback, `wifi_board.cc` prefix and hostname, and the `esp-wifi-connect` exact-SSID override. Confirm `config.h` M144 pins are unchanged before and after the diff.
2. **Device identity.** `application.cc` / `application.h` Change Wi-Fi marker and token preservation. No UI yet. Suppression flag is observable in logs.
3. **Shared UI hook.** `SpiLcdDisplay` `clear_color` default. CoreS3 keeps compiling with the existing constructor call.
4. **CoreS3 V4 behavior.** New 320×240 screens for boot, provisioning, waiting, binding, ready, main, listening, and speaking, using `SpiLcdDisplay` and FT6336. No Watcher coordinates. No ILI9342 or FT6336 driver edits.
5. **Nexus connection/status UI.** QR and Wi-Fi/battery/time row inside the 320×240 layout.
6. **Conversation state UI.** Listening and speaking driven by `SetDeviceState`.
7. **Notifications.** Status treatment for `kDeviceStateNotifying` only.
8. **Change Wi-Fi UI.** Confirm step that sets `cwifi_pend` and clears saved SSIDs without clearing websocket identity. Volume gestures stay unimplemented until a gesture is specified.
9. **MCP and reminders.** No code. Record that Watcher did not add any.
10. **Audio check.** Diff `config.h` and the M144 codec file against the pre-port revision. Pins and address `0x10` must be identical.

## Hardware still required on a physical CoreS3

- ILI9342 `disp_on_off` versus backlight-only sleep.
- FT6336 replacing the Watcher knob for Talk, volume, and Change Wi-Fi.
- M144 ES8388 capture and playback on Configuration B after the shared layer is linked.
- Nexus OTA and `wss://ws.nexus.warehouse-13.biz/xiaozhi/v1/` against a real device identity.
- First-boot binding versus Change Wi-Fi with an existing websocket token.
- 320×240 layout of the QR and status row. The 412×412 assets will not drop in.

## Phase 2–5 status

Branch `feature/cores3-watcher-parity` was created from Watcher `w13-sensecap` (`550eaed`) so this audit could be committed without touching `w13-sensecap`. No CoreS3 source was edited. `idf.py build` was not run, because no feature group was ported and the M144 tree is missing.
