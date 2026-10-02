#!/usr/bin/env python3
"""mkfont.py - TTF 에서 8x16 비트맵 콘솔 폰트를 뽑아 C 배열로 굽는다.

베어메탈에는 폰트 렌더러가 없다. 부팅 화면에 글씨를 찍으려면 글리프가
이미 비트맵이어야 한다. 그래서 빌드 시점에 미리 구워 소스에 박는다.

  ASCII 0x20~0x7E (95자) x 16바이트 = 1520 바이트

생성된 firmware/src/font_8x16.c 는 저장소에 커밋되어 있으므로 평소에는
이 스크립트를 돌릴 필요가 없다. 폰트를 바꾸고 싶을 때만 쓴다.

필요: Pillow  (apt install python3-pil  또는  pip install Pillow)

사용법:
    python3 tools/mkfont.py > firmware/src/font_8x16.c
    python3 tools/mkfont.py --preview        # 터미널로 결과 확인
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
CELL_W, CELL_H = 8, 16
FIRST, LAST = 0x20, 0x7E
THRESHOLD = 100          # 이 밝기 이상이면 켜진 픽셀로 본다
PIXEL_SIZE = 14          # 8px 셀에 맞는 DejaVu Sans Mono 크기
BASELINE_Y = 12          # 셀 안에서의 베이스라인 위치


def render_glyph(font, ch):
    """글자 하나를 8x16 비트맵(각 행 1바이트, MSB가 왼쪽)으로."""
    img = Image.new("L", (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    # anchor="ls" = left / baseline 기준. 셀마다 베이스라인을 맞춰야
    # 글자들이 위아래로 흔들리지 않는다.
    draw.text((0, BASELINE_Y), ch, fill=255, font=font, anchor="ls")

    rows = []
    px = img.load()
    for y in range(CELL_H):
        bits = 0
        for x in range(CELL_W):
            if px[x, y] >= THRESHOLD:
                bits |= 0x80 >> x
        rows.append(bits)
    return rows


def main():
    font = ImageFont.truetype(FONT_PATH, PIXEL_SIZE)
    glyphs = [(c, render_glyph(font, chr(c))) for c in range(FIRST, LAST + 1)]

    if "--preview" in sys.argv:
        for c, rows in glyphs:
            print(f"--- 0x{c:02X} '{chr(c)}' ---")
            for r in rows:
                print("".join("#" if r & (0x80 >> x) else "." for x in range(CELL_W)))
        return

    out = sys.stdout.write
    out("/* font_8x16.c - 자동 생성 파일. 직접 수정하지 말 것.\n"
        " *\n"
        " *   생성: python3 tools/mkfont.py > firmware/src/font_8x16.c\n"
        f" *   원본: {FONT_PATH}\n"
        f" *   범위: ASCII 0x{FIRST:02X}~0x{LAST:02X}, 셀 {CELL_W}x{CELL_H}\n"
        " *\n"
        " * 각 글자는 16바이트. 한 바이트가 한 행이고 MSB 가 왼쪽 픽셀이다. */\n"
        '#include "font.h"\n\n')
    out(f"const u8 font_8x16[FONT_GLYPH_COUNT][FONT_HEIGHT] = {{\n")
    for c, rows in glyphs:
        ch = chr(c)
        label = "'\\''" if ch == "'" else ("'\\\\'" if ch == "\\" else f"'{ch}'")
        body = ", ".join(f"0x{r:02X}" for r in rows)
        out(f"    {{ {body} }},  /* 0x{c:02X} {label} */\n")
    out("};\n")


if __name__ == "__main__":
    main()
