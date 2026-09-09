#!/usr/bin/env python3
# Convert the Calmati logo PNG (white glyph on black) into a 1-bit LVGL A1
# image, recoloured at draw time. Output: src/calmati_icon.c -> lv_image_dsc_t
# named `calmati_icon`.
from PIL import Image

IN_PATH   = r"C:\Users\cicci\Downloads\25190487.png"
OUT_PATH  = r"C:\Users\cicci\schermomoto\13-37\src\calmati_icon.c"
TARGET_W  = 120
THRESHOLD = 110   # white glyph on black -> luminance ABOVE this = opaque

img = Image.open(IN_PATH).convert("RGBA")
ow, oh = img.size
tw = TARGET_W
th = int(round(oh * TARGET_W / ow))
img = img.resize((tw, th), Image.LANCZOS)

stride_bytes = (tw + 7) // 8
data = bytearray(stride_bytes * th)
for y in range(th):
    for x in range(tw):
        r, g, b, a = img.getpixel((x, y))
        lum = (r * 30 + g * 59 + b * 11) // 100
        if a > 128 and lum > THRESHOLD:          # bright pixel -> mask=1
            data[y * stride_bytes + x // 8] |= (1 << (7 - (x % 8)))

data_size = len(data)
with open(OUT_PATH, "w") as f:
    f.write('/*\n'
            ' * Calmati logo - 1-bit alpha mask, %dx%d px, from the brand PNG\n'
            ' * via tools/gen_calmati_icon.py. Recoloured at draw time.\n'
            ' */\n\n'
            '#include "lvgl.h"\n\n'
            'static const LV_ATTRIBUTE_LARGE_CONST uint8_t CALMATI_ICON_DATA[] = {\n' % (tw, th))
    for i in range(0, data_size, 12):
        f.write("    " + ", ".join("0x%02x" % b for b in data[i:i+12]) + ",\n")
    f.write('};\n\n'
            'const lv_image_dsc_t calmati_icon = {\n'
            '    .header = {\n'
            '        .magic  = LV_IMAGE_HEADER_MAGIC,\n'
            '        .cf     = LV_COLOR_FORMAT_A1,\n'
            '        .flags  = 0,\n'
            '        .w      = %d,\n'
            '        .h      = %d,\n'
            '        .stride = %d,\n'
            '        .reserved_2 = 0,\n'
            '    },\n'
            '    .data_size = %d,\n'
            '    .data      = CALMATI_ICON_DATA,\n'
            '    .reserved   = NULL,\n'
            '    .reserved_2 = NULL,\n'
            '};\n' % (tw, th, stride_bytes, data_size))
print("wrote %s: %dx%d, %d B" % (OUT_PATH, tw, th, data_size))
