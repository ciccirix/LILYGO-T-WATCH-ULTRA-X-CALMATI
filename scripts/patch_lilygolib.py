# PlatformIO pre-build hook: apply the LilyGoLib source patches this firmware
# depends on, after lib_deps are fetched. These live inside .pio/libdeps/ (which
# is gitignored and reset by `pio run -t clean` or any library refetch), so
# without this hook a fresh checkout builds incorrectly or fails to compile.
# Idempotent and runs every build.
#
# Patches:
#   * SEND_BUF_SIZE 16384 -> 4096 in LilyGoDispInterface.cpp
#       Stock 32 KB SPI/DMA chunks fail with ESP_ERR_NO_MEM once WiFi
#       promiscuous + active BLE scan (wardriver) eat most of internal SRAM,
#       so screen redraws partially fail (wardriver->clock appears to freeze).
#       8 KB chunks fit in the SRAM left over. See README.
#   * LV_USE_SNAPSHOT 0 -> 1 in lv_conf.h
#       Required by screenshot.cpp (lv_snapshot_take); LilyGoLib ships it off.
Import("env")  # noqa: F821 (provided by the PlatformIO build environment)
import os
import re

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
pioenv  = env.subst("$PIOENV")
src_dir = os.path.join(libdeps, pioenv, "LilyGoLib", "src")

# (filename, #define name, required value)
PATCHES = [
    ("LilyGoDispInterface.cpp", "SEND_BUF_SIZE",   "(4096)"),
    ("lv_conf.h",               "LV_USE_SNAPSHOT", "1"),
]


def set_define(path, name, value):
    if not os.path.isfile(path):
        print("[patch_lilygolib] WARNING: %s not found; %s NOT patched" % (path, name))
        return
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    pat = re.compile(r"(#define\s+" + re.escape(name) + r"\s+)(\S+)")
    m = pat.search(src)
    if not m:
        print("[patch_lilygolib] WARNING: #define %s not found in %s" % (name, os.path.basename(path)))
        return
    if m.group(2) == value:
        print("[patch_lilygolib] %s already %s" % (name, value))
        return
    src = pat.sub(lambda mm: mm.group(1) + value, src, count=1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    print("[patch_lilygolib] %s: %s -> %s" % (name, m.group(2), value))


for fname, name, value in PATCHES:
    set_define(os.path.join(src_dir, fname), name, value)


# --- One-off include strip -----------------------------------------------
#
# `bsp_codec/esp_codec.cpp` includes both the modern I2S headers
# (`driver/i2s_std.h`, `i2s_pdm.h`, `i2s_tdm.h`) AND the legacy
# `driver/i2s.h`. On arduino-esp32 3.3.11 (pioarduino), `ESP_I2S.h` from the
# Arduino ESP32 core defines `i2s_mode_t`, and so does the legacy
# `i2s_types_legacy.h` pulled in by `driver/i2s.h` — resulting in
# `conflicting declaration 'typedef enum i2s_mode_t i2s_mode_t'`. The legacy
# include is not needed (the file already uses the new API), so strip it.
def strip_include(path, needle):
    if not os.path.isfile(path):
        print("[patch_lilygolib] WARNING: %s not found; include NOT stripped" % path)
        return
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    pat = re.compile(r"^[ \t]*#include\s+[\"<]" + re.escape(needle) + r"[\">][^\n]*\n",
                     re.MULTILINE)
    new_src, n = pat.subn("", src)
    if n == 0:
        print("[patch_lilygolib] include \"%s\" already absent in %s" % (needle, os.path.basename(path)))
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_src)
    print("[patch_lilygolib] stripped %d include of \"%s\" from %s" % (n, needle, os.path.basename(path)))


strip_include(os.path.join(src_dir, "bsp_codec", "esp_codec.cpp"), "driver/i2s.h")
