# `esp-sr-poc` — offline voice-control proof of concept

This branch is **experimental**. It bumps the PlatformIO platform from
`espressif32@6.10.0` (platformio/platformio, IDF 5.1) to
`pioarduino/platform-espressif32 55.03.311` (arduino-esp32 3.3.11,
IDF 5.4) so we can pick up Espressif's `ESP_SR` Arduino component —
which ships `esp32-hal-sr.h`, a pre-shipped `wn9_hiesp` WakeNet model,
and the built-in `esp_sr_16` partition scheme — in three lines of
config instead of fifteen.

**Main is not affected.** If this branch ends up unbuildable or the
end-to-end result is worse than what we have on main, we throw it away
and try the more conservative route (esp-sr as an IDF managed component
under the existing IDF 5.1 platform).

See [`docs/esp-sr-plan.md`](docs/esp-sr-plan.md) for the full research
and the decision log.

## Success criteria for this branch

1. Bump the platform. The **existing 41 tiles must still compile** on
   the new platform — no regression allowed. This is step 0.
2. Add the `esp_sr_16` partition scheme + `esp32-hal-sr.h` include in
   `voice_screen.cpp` (or a fresh `wake_word.cpp`). Wake-word "Hi ESP"
   detected via the same mic we already use for `mic_rec`.
3. Wire the KITT bars in the Assist tile to the WakeNet audio energy
   so the operator sees the wake-word listener is alive.
4. Extend to MultiNet6 with 10-12 English commands, each mapping to
   one of the existing `_screen_show()` calls (Italian UI remains).

If step 1 succeeds this branch merges into main behind a build gate.
If step 1 fails and can't be fixed inside a session, we abandon this
branch and go the IDF-component route on main directly.
