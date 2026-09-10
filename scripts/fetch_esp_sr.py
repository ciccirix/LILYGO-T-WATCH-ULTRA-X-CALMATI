# PlatformIO pre-build hook: populate lib/esp-sr/ (the vendored ESP-SR
# offline speech-recognition libs) on first checkout so the .a archives
# and packed model don't bloat git history.
#
# On every build:
#   * Ensures lib/esp-sr/libs/esp32s3/*.a exist. If any are missing, the
#     esp-sr@1.9.5 release archive is downloaded from Espressif's Component
#     Registry (~100 MB), and only the S3 archives + headers we need are
#     extracted into lib/esp-sr/. First run is slow; every subsequent run
#     is a stat-only no-op.
#   * Ensures lib/esp-sr/models/srmodels.bin exists. If missing, it's
#     re-packed from model/wakenet_model/wn9_hiesp using the release's
#     own pack_model.py — no ESP-IDF environment needed.
#
# See docs/esp-sr-plan.md for why the release is vendored instead of pulled
# in as an IDF managed component.
Import("env")  # noqa: F821 (provided by the PlatformIO build environment)
import os
import shutil
import sys
import tempfile
import zipfile
try:
    from urllib.request import urlopen
except ImportError:
    from urllib2 import urlopen  # py2 fallback

PROJECT_DIR = env.subst("$PROJECT_DIR")
VENDOR_DIR  = os.path.join(PROJECT_DIR, "lib", "esp-sr")

# Espressif Component Registry direct download for esp-sr 1.9.5. The object
# id was captured from the versions page — the URL scheme is stable across
# releases, but the id is per-version.
ESP_SR_URL = (
    "https://components.espressif.com/api/downloads/"
    "?object_type=component&object_id=f5dd3c3a-83cf-4856-8f82-7f2bc0a6259f"
)

# .a archives we ship for the S3 target — the full set that WakeNet + AFE
# pull in through their intra-archive refs. Skipping any of these ends in
# a link-time "undefined reference".
REQUIRED_LIBS = [
    "libwakenet.a",
    "libmultinet.a",
    "libesp_audio_front_end.a",
    "libesp_audio_processor.a",
    "libnsnet.a",
    "libhufzip.a",
    "libc_speech_features.a",
    "libdl_lib.a",
    "libfst.a",
    "libflite_g2p.a",
]

LIBS_DIR   = os.path.join(VENDOR_DIR, "libs", "esp32s3")
INC_DIR    = os.path.join(VENDOR_DIR, "include", "esp-sr")
MODEL_BIN  = os.path.join(VENDOR_DIR, "models", "srmodels.bin")


def libs_present():
    if not os.path.isdir(LIBS_DIR):
        return False
    for name in REQUIRED_LIBS:
        if not os.path.isfile(os.path.join(LIBS_DIR, name)):
            return False
    return True


def download_and_extract():
    print("[fetch_esp_sr] downloading esp-sr@1.9.5 archive (~100 MB)...")
    tmp = tempfile.mkdtemp(prefix="esp-sr-fetch-")
    zpath = os.path.join(tmp, "esp-sr.zip")
    try:
        with urlopen(ESP_SR_URL) as r, open(zpath, "wb") as f:
            shutil.copyfileobj(r, f)
        print("[fetch_esp_sr] extracting relevant subset...")
        with zipfile.ZipFile(zpath) as z:
            for member in z.namelist():
                # Only the S3 libs, matching-target headers, and the src
                # includes we actually reference from wake_word.cpp.
                if member.startswith("esp-sr/lib/esp32s3/") and member.endswith(".a"):
                    dest = os.path.join(LIBS_DIR, os.path.basename(member))
                    _extract_one(z, member, dest)
                elif member.startswith("esp-sr/include/esp32s3/") and member.endswith(".h"):
                    dest = os.path.join(INC_DIR, os.path.basename(member))
                    _extract_one(z, member, dest)
                elif member.startswith("esp-sr/src/include/") and member.endswith(".h"):
                    dest = os.path.join(INC_DIR, os.path.basename(member))
                    _extract_one(z, member, dest)
                elif member == "esp-sr/src/model_path.c":
                    dest = os.path.join(VENDOR_DIR, "src", "model_path.c")
                    _extract_one(z, member, dest)
                elif member == "esp-sr/model/pack_model.py":
                    dest = os.path.join(tmp, "pack_model.py")
                    _extract_one(z, member, dest)
                elif member.startswith("esp-sr/model/wakenet_model/wn9_hiesp/"):
                    rel = member.replace("esp-sr/model/wakenet_model/", "", 1)
                    dest = os.path.join(tmp, "models_src", rel)
                    _extract_one(z, member, dest)

        # Pack srmodels.bin from wn9_hiesp — 285 KB output, ships alone.
        models_src = os.path.join(tmp, "models_src")
        pack_py = os.path.join(tmp, "pack_model.py")
        os.makedirs(os.path.dirname(MODEL_BIN), exist_ok=True)
        print("[fetch_esp_sr] packing srmodels.bin (wn9_hiesp only)...")
        # pack_model.py writes out_file next to model_path — do it in the
        # tmp models_src, then move.
        env.Execute("\"%s\" \"%s\" -m \"%s\" -o srmodels.bin" %
                    (sys.executable, pack_py, models_src))
        packed = os.path.join(models_src, "srmodels.bin")
        if os.path.isfile(packed):
            shutil.copyfile(packed, MODEL_BIN)
        else:
            print("[fetch_esp_sr] WARNING: srmodels.bin was not produced")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _extract_one(z, member, dest_path):
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    with z.open(member) as src, open(dest_path, "wb") as out:
        shutil.copyfileobj(src, out)


if libs_present() and os.path.isfile(MODEL_BIN):
    print("[fetch_esp_sr] vendored libs + srmodels.bin already present, skipping")
else:
    download_and_extract()
    if libs_present():
        print("[fetch_esp_sr] all %d libs installed" % len(REQUIRED_LIBS))
    else:
        print("[fetch_esp_sr] WARNING: some required libs are still missing")
