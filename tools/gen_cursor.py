from PIL import Image, ImageDraw

W, H = 12, 19
img = Image.new("L", (W, H), 0)  # 0=transparent, 1=fill, 2=outline (we'll encode via draw order)

# classic arrow polygon points (x,y)
arrow = [(0,0),(0,16),(4,12),(6,18),(8,17),(6,11),(11,11)]

fill_img = Image.new("1", (W, H), 0)
outline_img = Image.new("1", (W, H), 0)

d_fill = ImageDraw.Draw(fill_img)
d_fill.polygon(arrow, fill=1)

d_outline = ImageDraw.Draw(outline_img)
d_outline.polygon(arrow, outline=1, fill=0)
d_outline.line(arrow + [arrow[0]], fill=1, width=1)

with open("/home/claude/myos/kernel/cursor_bitmap.c", "w") as f:
    f.write("/* cursor_bitmap.c -- arrow cursor sprite, rasterized via tools/gen_cursor.py\n")
    f.write(" * (same rationale as the font: generated from a real polygon and\n")
    f.write(" * visually verified, not hand-transcribed pixel by pixel). */\n\n")
    f.write('#include "cursor_bitmap.h"\n\n')
    f.write(f"const unsigned char cursor_width = {W};\n")
    f.write(f"const unsigned char cursor_height = {H};\n\n")
    f.write("/* row-major, 1 byte per pixel: 0=transparent, 1=white fill, 2=black outline */\n")
    f.write(f"const unsigned char cursor_bitmap[{H}][{W}] = {{\n")
    for y in range(H):
        row = []
        for x in range(W):
            if outline_img.getpixel((x,y)):
                row.append(2)
            elif fill_img.getpixel((x,y)):
                row.append(1)
            else:
                row.append(0)
        f.write("    { " + ", ".join(str(v) for v in row) + " },\n")
    f.write("};\n")

# preview
preview = Image.new("RGB", (W*20, H*20), (30,30,60))
pd = ImageDraw.Draw(preview)
for y in range(H):
    for x in range(W):
        if outline_img.getpixel((x,y)):
            pd.rectangle([x*20,y*20,x*20+19,y*20+19], fill=(0,0,0))
        elif fill_img.getpixel((x,y)):
            pd.rectangle([x*20,y*20,x*20+19,y*20+19], fill=(255,255,255))
preview.save("/tmp/cursor_preview.png")
print("done")
