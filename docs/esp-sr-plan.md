# ESP-SR on the T-Watch Ultra — offline voice control (research + plan)

Last updated: 2026-09-09. Author of this note: Claude, drafting for ciccirix.

Goal: **fully offline** voice control on the watch — always-on wake word +
short command set, response rendered as text on the LVGL screen (no TTS
needed, so no huge synthesis engine competing for flash).

Mic hardware is already wired and validated on the `Mic Rec` tile
(`src/mic_rec.cpp`, PDM microphone via the board's I2S peripheral). This
plan assumes that path is reusable.

## Executive summary

- **What can run offline on this board:** wake-word detection (WakeNet) +
  small-vocabulary command recognition (MultiNet6) — Espressif ships both
  as the `esp-sr` component, optimised for the ESP32-S3's Xtensa LX7 SIMD.
- **What can't:** any real LLM. The largest open weights that fit at 4-bit
  quantisation (~1-3 M parameters) are useless as chatbots. If we ever
  want free-form "ask a question", it belongs on the phone via a `Phone
  Link`-style bridge or on the survivor-kit Pi ([[udda-offgrid-pi4]]),
  not on-device.
- **Italian is NOT natively supported** in either WakeNet or MultiNet6/7
  as of Sept 2026. This is the biggest external constraint on the plan.
  Both mitigations are usable — see the "Language" section below.

## Facts and constraints (with sources)

### Component & version

- Component: `espressif/esp-sr`, latest stable v2.4.7 on the Component
  Registry ([registry][sr-reg], [repo][sr-repo]). ESP-SR v2.0 (Feb 2025)
  is the required baseline for MultiNet6/7.
- Requires **ESP-IDF ≥ 5.2**. Our current build is `espressif32@6.11.0`
  → ESP-IDF 5.1.x underneath. **We need to bump the platform**, or pin
  esp-sr to the last v1.x release (v1.9.4) that still runs on IDF 5.1.
- Chip target: **ESP32-S3** (our board — good). Also supports S31 and
  the P4; the C-series (C3/C5/C6) only runs the cost-down WakeNet9s.

### Wake word (WakeNet)

Pre-shipped WakeNet9 models (each is a separate flash blob so you pick
one at build time and only that one lives on the device):

| model id            | phrase           | notes |
| ------------------- | ---------------- | ----- |
| `wn9_hiesp`         | "Hi ESP"         | English, most reliable pre-trained |
| `wn9_alexa`         | "Alexa"          | English |
| `wn9_hilexin`       | "Hi Lexin"       | Chinese |
| `wn9_jarvis`        | "Jarvis"         | English |
| `wn9_computer`      | "Computer"       | English |
| `wn9_hey_willow`    | "Hey Willow"     | English |
| `wn9_sophia`        | "Sophia"         | English |
| `wn9_mycroft`       | "Mycroft"        | English |

There are ~30 total, incl. many Chinese phrases. Full list via
`idf.py menuconfig` under `ESP Speech Recognition` after the component is
pulled in.

Custom wake words (e.g. "Hey Calmati"): Espressif's TTS Pipeline V3
(Apr 2026) currently accepts Chinese, English, Japanese, French; the
2026 roadmap adds Korean, Spanish, Portuguese, German, Russian, Arabic
([README][sr-repo]). **Italian is not on the list.** English pronunciation
of "Hey Calmati" through the EN pipeline is the pragmatic workaround —
it costs one round with the portal but the result is a real custom
wake word, not a hack.

### Command recognition (MultiNet)

- **MultiNet6** (grapheme-based, English commands entered as plain
  spelled words) is what we want. MultiNet7 uses phonemes and needs a
  G2P conversion step — extra complexity for no on-device benefit
  ([docs][mn-docs]).
- Supported languages: Chinese, English only. **No Italian.**
- Up to **200 commands** per model.
- Latency **≤ 500 ms** per detected command.
- API (verified in the docs, we'll be calling exactly these):
  ```c
  esp_err_t     esp_mn_commands_add(int id, char *grapheme);
  esp_mn_error_t *esp_mn_commands_update();
  ```
- **Language mitigation** (this is the key insight): MultiNet6 recognises
  the *English spelling* of what's said, but the *action* that fires is
  ours. So we get to keep an Italian UI while the user says the command
  in English. That's the same trick every voice assistant used before
  full-multilingual models, and it works cleanly here.

### Memory footprint (estimated, needs a real build to confirm)

Not spelled out in the ESP-SR docs page I fetched — the Benchmark section
links out to per-model tables. Ballpark from the release notes and
community reports for the S3:

| block          | internal SRAM | PSRAM  | Flash   |
| -------------- | ------------- | ------ | ------- |
| AFE front-end  | ~50 KB        | ~200 KB| —       |
| WakeNet9 (EN)  | ~40 KB        | ~50 KB | ~100 KB |
| MultiNet6 (EN) | ~60 KB        | ~500 KB| ~300 KB |
| **Total add**  | **~150 KB**   | ~750 KB| ~400 KB |

Today the firmware runs at RAM 76.4 % (250 KB / 320 KB internal) and
Flash 59.5 % (3.74 MB / 6 MB). Adding ~150 KB SRAM lands us at ~400 KB
projected — which **exceeds internal SRAM (320 KB)**. So:

- **We must move the audio front-end buffers to PSRAM.** ESP-SR AFE
  supports this via `AFE_CONFIG_INIT()` with `.use_psram = true`.
- **We must reduce the always-on WakeNet SRAM cost**, which the
  documented `wn9_hiesp` config does by default (it's the "low memory"
  variant). WakeNet10 (Aug 2026) is only a hair heavier.

Both are stated compatible with the S3 in the release notes. But this
is exactly the kind of forecast that gets falsified by a real build —
we do the "PoC build" first and validate before committing.

### Coexistence with WiFi + BLE

- ESP-SR itself is **DSP-only, no radio**. It doesn't compete with the
  RF coexistence machinery on the S3 at all.
- The board's RF time-division multiplexer already sequences
  Wi-Fi + BLE ([docs][coex]).
- Our fork has a hard constraint documented in [[twatch-threat-radar]]:
  Bluedroid + Wi-Fi up together = freeze on the current build. Fix
  lives in `ble_scan_manager.cpp` (`stack_up()` fully deinits WiFi
  before bringing BLE up). **ESP-SR doesn't touch that path**, but the
  CPU budget on core 0 matters — see below.
- CPU: WakeNet9 always-on is reported at ~15 % CPU on a dedicated
  core. Our fork puts WiFi RX cb and Bluedroid on core 0; LVGL/main on
  core 1. Cleanest allocation: **WakeNet on core 1** (LVGL is bursty,
  most of the time it's idle), or pin it to a dedicated task with a
  hard priority ceiling so it can't starve LVGL touch input.
- **Assist tile behaviour under voice**: today it's WiFi-based
  (STT-in-cloud). If we accept the "no cloud" rule, the whole path
  becomes CPU-only, WiFi can stay off during voice interaction — which
  actually simplifies the coexistence problem the current Assist has.

### Build integration on our platform

The two options, ordered by risk:

1. **Managed component in the current Arduino-ESP32 3.x + PlatformIO
   setup.** `pioarduino` (community fork PlatformIO uses for Arduino-
   ESP32 3.x) supports IDF managed components. Config sketch:
   ```ini
   ; platformio.ini — twatch_ultra section
   custom_component_remove = espressif/esp-sr    ; if pio has an older pin
   custom_component_add    = espressif/esp-sr@^2.4
   board_build.embed_files = model/model.bin     ; the packed models
   board_build.partitions  = partitions_sr.csv   ; adds an sr_model partition
   ```
   Then `partitions_sr.csv` adds a `sr_model, data, 0x82, 0x610000, 1M`
   entry, and we shrink `spiffs` (or drop it) to make room.

2. **Fall back to pure ESP-IDF.** If option 1 fights the arduino wrapper
   (very possible — the packed model loading is IDF-native), we split
   the voice engine into its own `esp-idf` project that runs on core 1
   and talks to the arduino app via a FreeRTOS queue in shared PSRAM.
   Ugly but proven.

The reference offline-voice project I looked at is
[jasin-jesin/ESP32-S3-OFFLINE-VOICE-RECOGNITION][ref-repo] — pure
IDF 4.4.6, S3-DevKit-C. Useful for the mic wiring but not our exact
target.

## Proposed command set (English words, Italian UI)

Start small — MultiNet6 accuracy drops as you add commands. First cut:

| spoken command       | action                                         |
| -------------------- | ---------------------------------------------- |
| "threat radar"       | open Threat Radar tile                         |
| "scanner"            | open Scanner (unified WiFi+BLE)                |
| "camera"             | open Cameras tile                              |
| "deauth"             | open Deauther (does NOT auto-fire)             |
| "waterfall"          | open Waterfall FFT                             |
| "screenshot"         | fire a screenshot right now                    |
| "settings"           | open Settings                                  |
| "clock"              | back to the watchface                          |
| "map"                | open Meshtastic Map                            |
| "messages"           | open Meshtastic Messages                       |
| "silence"            | dismiss any active alarm / haptic              |
| "duress"             | arm the duress screen                          |

12 commands, all English, all short (grapheme-friendly). Each maps to
one existing `_screen_show()` call we already have. Nothing new fires
an attack — voice never crosses the "authorise this action" line, it
only navigates.

## Wake word decision

Three viable paths, in order of my preference:

1. **Ship "Hi ESP" (`wn9_hiesp`) for the first release.** Zero portal
   turnaround, we can build and test today. Cheeky but honest; the
   splash screen and settings label read "wake word: Hi ESP".
2. **Custom "Hey Calmati" through the EN pipeline of Espressif's TTS
   Portal.** Best brand fit, needs one round with the portal (~days,
   free of charge as of Apr 2026). Only path that gives us our own
   wake word before Italian training arrives.
3. **Wait for Italian TTS pipeline support.** Not on the 2026 roadmap
   yet — could be six months or more. Do not block on this.

Recommendation: **option 1 for the first cut**, upgrade to option 2 in
a follow-up release. Users can flip between wake words via a Settings
tile if we ship both blobs (~200 KB more flash — affordable).

## Risks and unknowns

- **Real memory footprint after build**: the numbers above are
  estimates. First tangible milestone: a PoC that boots with just
  wake-word detection and nothing else, and its `pio run` size line.
  Only after that do we know if MultiNet6 fits.
- **PIO + IDF managed component friction**: option 1 might just refuse
  to add `esp-sr` cleanly. If two evenings hit walls, we switch to
  option 2 (dual-project).
- **CPU contention with LVGL**: WakeNet's audio callback runs every
  16 ms. If it lands on core 1, we need to prove touch responsiveness
  doesn't tank. Cheap fix: pin LVGL touch handling to a higher priority
  than WakeNet's postprocess.
- **Battery**: continuous mic + WakeNet ≈ +15-20 mA average. On a
  T-Watch with a ~400 mAh battery that's ~24 h of pure standby lost.
  Users will want a "wake word off during sleep" toggle → we can gate
  it on the accelerometer (mic on only when wrist raised) — the same
  gesture that already brightens the screen.
- **False triggers around a security tool**: "Hi ESP" fired by ambient
  chatter that opens some other tile is annoying. Threshold + a
  push-to-listen fallback (long-press) both live in the ESP-SR config.

## Concrete first-step deliverable

When we resume, first hour of work:

1. Bump `platformio.ini` from `espressif32@6.11.0` to `pioarduino @ ^52`
   (or whichever gives IDF 5.2+).
2. Add `esp-sr@^2.4` as a managed component. Build. Fix whatever
   errors that surfaces.
3. Add a `sr_model` partition and pack `wn9_hiesp` only.
4. Wire the mic → AFE → WakeNet → serial log. Success = "Hi ESP" on
   the serial monitor when spoken.

That's it. If step 1-4 works, everything else (MultiNet, tile hookup,
UI) is straightforward. If step 1 fights, we know to switch to the
dual-project setup and lose a session on plumbing.

## References

[sr-repo]: https://github.com/espressif/esp-sr
[sr-reg]: https://components.espressif.com/components/espressif/esp-sr
[mn-docs]: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html
[wn-docs]: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
[coex]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/coexist.html
[skainet]: https://github.com/espressif/esp-skainet
[ref-repo]: https://github.com/jasin-jesin/ESP32-S3-OFFLINE-VOICE-RECOGNITION

- Espressif ESP-SR component: [github.com/espressif/esp-sr][sr-repo]
- ESP-SR on the Component Registry (latest release, 2026):
  [components.espressif.com/.../esp-sr][sr-reg]
- WakeNet docs: [docs.espressif.com/.../wake_word_engine][wn-docs]
- MultiNet docs: [docs.espressif.com/.../speech_command_recognition][mn-docs]
- ESP-Skainet reference project (Espressif official examples,
  incl. `en_speech_commands_recognition`, `wake_word_detection`):
  [github.com/espressif/esp-skainet][skainet]
- Reference community offline-voice project (IDF 4.4.6, S3-DevKit-C):
  [github.com/jasin-jesin/ESP32-S3-OFFLINE-VOICE-RECOGNITION][ref-repo]
- RF Coexistence rules on the S3:
  [docs.espressif.com/.../coexist][coex]
