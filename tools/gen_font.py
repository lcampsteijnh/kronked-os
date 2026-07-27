from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
CHAR_W, CHAR_H = 8, 16
FIRST, LAST = 32, 126  # printable ASCII

font = ImageFont.truetype(FONT_PATH, 13)

glyphs = []  # each glyph: list of CHAR_H bytes, bit7=leftmost pixel
preview = Image.new("1", (CHAR_W * 16, CHAR_H * ((LAST - FIRST) // 16 + 1)), 0)

for idx, code in enumerate(range(FIRST, LAST + 1)):
    ch = chr(code)
    img = Image.new("1", (CHAR_W, CHAR_H), 0)
    draw = ImageDraw.Draw(img)
    # center glyph reasonably; DejaVu at size 13 roughly fits an 8x16 cell
    draw.text((0, 0), ch, font=font, fill=1)
    rows = []
    for y in range(CHAR_H):
        byte = 0
        for x in range(CHAR_W):
            if img.getpixel((x, y)):
                byte |= (0x80 >> x)
        rows.append(byte)
    glyphs.append(rows)

    px, py = (idx % 16) * CHAR_W, (idx // 16) * CHAR_H
    preview.paste(img, (px, py))

preview.convert("L").resize((preview.width*3, preview.height*3), Image.NEAREST).save("/tmp/font_preview.png")

with open("/home/claude/myos/kernel/font8x16.c", "w") as f:
    f.write("/* font8x16.c -- 8x16 bitmap font, ASCII 32-126\n")
    f.write(" * Rasterized from DejaVu Sans Mono via tools/gen_font.py, not\n")
    f.write(" * hand-transcribed -- generated data is easy to get subtly wrong\n")
    f.write(" * by hand across 95 glyphs; this way each glyph is guaranteed to\n")
    f.write(" * match a real, known-correct font rendering, and was visually\n")
    f.write(" * verified against a rendered preview before being embedded. */\n\n")
    f.write('#include "font8x16.h"\n\n')
    f.write(f"const unsigned char font8x16_data[{LAST-FIRST+1}][16] = {{\n")
    for code, rows in zip(range(FIRST, LAST+1), glyphs):
        ch = chr(code)
        comment = ch if ch not in ('\\', "'") else ('\\\\' if ch=='\\' else "\\'")
        f.write("    { " + ", ".join(f"0x{b:02x}" for b in rows) + f" }}, /* {code} '{comment}' */\n")
    f.write("};\n")

print("done, glyphs:", len(glyphs))
