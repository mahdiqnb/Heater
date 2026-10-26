#!/usr/bin/env python3
"""Host-side mockup renderer for the STM32 UI.

This is NOT the firmware: it re-implements the same layout constants and
drawing primitives as Core/Src/ui.c so the geometry can be checked (and
looked at) without flashing the board.  Every number here mirrors the C code.
"""
import re
import zlib
import struct
import math
import sys
import os

W, H = 320, 240

# ---------------------------------------------------------------- palette
UI_BG = 0x0842
UI_CARD = 0x10A4
UI_SEL = 0x2106
UI_EDGE = 0x2967
UI_LINE = 0x2124
UI_GOLD = 0xE567
UI_GOLD_D = 0x8B44
UI_GHOST = 0x18C3
UI_TEXT = 0xE77E
UI_MUTED = 0x8C74
UI_GREEN = 0x44AF
UI_RED = 0xEA48
UI_ORANGE = 0xFCC7
UI_ORANGE_D = 0x4962
UI_BLUE = 0x3D5F

# ---------------------------------------------------------------- layout
HDR_H = 28
FOOT_Y = 212
FOOT_H = 26
TC_X, TC_Y, TC_W, TC_H = 6, 32, 164, 107
MC_Y, MC_H = 143, 64
GRF_X, GRF_Y, GRF_W, GRF_H = 176, 32, 140, 175
PLOT_X, PLOT_Y, PLOT_W, PLOT_H = 184, 50, 126, 151
DIG_Y, DIG_W, DIG_H, DIG_TH = 46, 22, 38, 6
SETUP_TOP, SETUP_RH = 32, 34

PAGE_HOME, PAGE_SETUP, PAGE_ABOUT = 0, 1, 2


def load_font(path):
    txt = open(path, encoding="utf-8", errors="ignore").read()
    body = txt.split("font5x7[95][7] = {", 1)[1]
    rows = re.findall(r"\{([0-9xA-Fa-f,\s]+)\}", body)
    glyphs = []
    for r in rows:
        vals = [int(v, 16) for v in r.replace("\n", "").split(",") if v.strip()]
        if len(vals) == 7:
            glyphs.append(vals)
    if len(glyphs) != 95:
        raise SystemExit("font parse: got %d glyphs" % len(glyphs))
    return glyphs


class Screen:
    def __init__(self):
        self.px = [0] * (W * H)

    def fill_rect(self, x, y, w, h, c):
        if w <= 0 or h <= 0:
            return
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + w), min(H, y + h)
        for yy in range(y0, y1):
            base = yy * W
            for xx in range(x0, x1):
                self.px[base + xx] = c

    def pixel(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y * W + x] = c

    # ---- primitives (mirror of ui.c) ----
    def line(self, x0, y0, x1, y1, c):
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.pixel(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def tri(self, x, y, w, h, c, d):
        for i in range(w):
            hh = 1 + (w - 1 - i) * (h - 1) // (w - 1 if w > 1 else 1)
            yy = y + (h - hh) // 2
            self.fill_rect(x + (i if d > 0 else w - 1 - i), yy, 1, hh, c)

    def rrect(self, x, y, w, h, c):
        ins = [3, 1, 0, 0]
        if w <= 0 or h <= 0:
            return
        if h < 8 or w < 10:
            self.fill_rect(x, y, w, h, c)
            return
        for i in range(4):
            d = ins[i]
            self.fill_rect(x + d, y + i, w - 2 * d, 1, c)
            self.fill_rect(x + d, y + h - 1 - i, w - 2 * d, 1, c)
        self.fill_rect(x, y + 4, w, h - 8, c)

    def panel(self, x, y, w, h, bg, edge):
        self.rrect(x, y, w, h, edge)
        self.rrect(x + 1, y + 1, w - 2, h - 2, bg)

    def char(self, x, y, ch, fg, bg, sc):
        if ord(ch) < 32 or ord(ch) > 126:
            ch = "?"
        g = FONT[ord(ch) - 32]
        w, h = 6 * sc, 7 * sc
        if x + w > W or y + h > H:
            return
        for row in range(7):
            bits = g[row]
            for sy in range(sc):
                for col in range(5):
                    c = fg if (bits & (1 << (4 - col))) else bg
                    for sx in range(sc):
                        self.pixel(x + col * sc + sx, y + row * sc + sy, c)
                for sx in range(sc):
                    self.pixel(x + 5 * sc + sx, y + row * sc + sy, bg)

    def text(self, x, y, s, fg, bg, sc):
        cx = x
        for ch in s:
            self.char(cx, y, ch, fg, bg, sc)
            cx += 6 * sc

    def text_w(self, s, sc):
        return len(s) * 6 * sc

    def text_c(self, cx, y, s, fg, bg, sc):
        self.text(cx - self.text_w(s, sc) // 2, y, s, fg, bg, sc)

    def text_r(self, rx, y, s, fg, bg, sc):
        self.text(rx - self.text_w(s, sc), y, s, fg, bg, sc)


def shade(c, pct):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r * pct // 100) << 11) | ((g * pct // 100) << 5) | (b * pct // 100)


def meter_bar(s, x, y, w, h, pct, c):
    s.fill_rect(x, y, w, h, UI_LINE)
    pct = min(pct, 100)
    fw = w * pct // 100
    if fw <= 0:
        return
    for i in range(h):
        b = 100 - (i * 40) // (h - 1 if h > 1 else 1)
        s.fill_rect(x, y + i, fw, 1, shade(c, b))


SA, SB, SC_, SD, SE, SF, SG = 1, 2, 4, 8, 0x10, 0x20, 0x40
DIG_SEG = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F]


def seg_mask(s, x, y, w, h, th, m, c):
    mh = (h - th) // 2
    bh = mh - th
    if m & SA:
        s.fill_rect(x + th, y, w - 2 * th, th, c)
    if m & SG:
        s.fill_rect(x + th, y + (h - th) // 2, w - 2 * th, th, c)
    if m & SD:
        s.fill_rect(x + th, y + h - th, w - 2 * th, th, c)
    if m & SF:
        s.fill_rect(x, y + th, th, bh, c)
    if m & SB:
        s.fill_rect(x + w - th, y + th, th, bh, c)
    if m & SE:
        s.fill_rect(x, y + h - mh, th, bh, c)
    if m & SC_:
        s.fill_rect(x + w - th, y + h - mh, th, bh, c)


def seg_digit(s, x, y, w, h, th, d, on, off):
    seg_mask(s, x, y, w, h, th, 0x7F, off)
    seg_mask(s, x, y, w, h, th, DIG_SEG[d % 10], on)


def brand(s, cx, cy, r, col):
    hw = r
    hh = r * 7 // 8
    s.line(cx - hw, cy, cx - hw // 2, cy - hh, col)
    s.line(cx - hw // 2, cy - hh, cx + hw // 2, cy - hh, col)
    s.line(cx + hw // 2, cy - hh, cx + hw, cy, col)
    s.line(cx + hw, cy, cx + hw // 2, cy + hh, col)
    s.line(cx + hw // 2, cy + hh, cx - hw // 2, cy + hh, col)
    s.line(cx - hw // 2, cy + hh, cx - hw, cy, col)
    for k in range(2):
        w2 = r * 3 // 5 - k * (r // 4)
        vy = cy - r // 6 + k * (r // 3)
        s.line(cx - w2, vy - r // 4, cx, vy, col)
        s.line(cx, vy, cx + w2, vy - r // 4, col)


def trace(s, x, y, ln, d, c):
    ex = x + d * ln
    s.fill_rect(min(x, ex), y, ln, 1, c)
    s.rrect(ex - 2, y - 2, 5, 5, c)


# ---------------------------------------------------------------- chrome
def draw_status(s, st):
    lbl = ["STOP", "RUN ", "ERR ", "IDLE"][st]
    col = [UI_MUTED, UI_GREEN, UI_RED, UI_GOLD_D][st]
    s.rrect(238, 7, 74, 16, col)
    s.rrect(239, 8, 72, 14, UI_CARD)
    s.text_c(275, 12, lbl, col, UI_CARD, 1)


def draw_header(s, title, st):
    s.fill_rect(0, 0, W, HDR_H, UI_CARD)
    s.fill_rect(0, HDR_H - 2, W, 1, UI_EDGE)
    s.fill_rect(0, HDR_H - 2, 46, 2, UI_GOLD)
    brand(s, 18, 14, 9, UI_GOLD)
    s.text(34, 7, title, UI_GOLD, UI_CARD, 2)
    s.text(178, 11, "V2.0", UI_MUTED, UI_CARD, 1)
    draw_status(s, st)


def draw_footer(s, page, selected):
    names = ["HOME", "SETTINGS", "ABOUT"]
    bg = UI_SEL if selected else UI_CARD
    edge = UI_GOLD if selected else UI_EDGE
    s.rrect(6, FOOT_Y, 308, FOOT_H, edge)
    s.rrect(7, FOOT_Y + 1, 306, FOOT_H - 2, bg)
    s.tri(16, FOOT_Y + 8, 6, 10, UI_GOLD_D, +1)
    s.tri(298, FOOT_Y + 8, 6, 10, UI_GOLD_D, -1)
    s.text_c(160, FOOT_Y + 6, names[page], UI_GOLD if selected else UI_TEXT, bg, 2)
    for i in range(3):
        s.fill_rect(152 + i * 8, FOOT_Y + 20, 3, 3, UI_GOLD if i == page else UI_EDGE)


# ---------------------------------------------------------------- home
def home_digit(s, slot, val, blank):
    xs = [28, 54, 80, 110]
    if blank:
        s.fill_rect(xs[slot], DIG_Y, DIG_W, DIG_H, UI_CARD)
        return
    seg_digit(s, xs[slot], DIG_Y, DIG_W, DIG_H, DIG_TH, val, UI_GOLD, UI_GHOST)


def home_temp(s, t10):
    home_digit(s, 0, (t10 // 1000) % 10, t10 < 1000)
    home_digit(s, 1, (t10 // 100) % 10, t10 < 100)
    home_digit(s, 2, (t10 // 10) % 10, t10 < 10)
    home_digit(s, 3, t10 % 10, False)


def home_set_block(s, sp10, selected):
    bg = UI_SEL if selected else UI_CARD
    s.rrect(9, 99, 158, 29, UI_GOLD if selected else UI_EDGE)
    s.rrect(10, 100, 156, 27, bg)
    s.text(16, 104, "SET", UI_GOLD_D if selected else UI_MUTED, bg, 1)
    s.text_r(158, 107, "%d.%d C" % (sp10 // 10, sp10 % 10),
             UI_GOLD if selected else UI_TEXT, bg, 2)


def home_meter(s, idx, pct, selected):
    names = ["HEAT", "IRON", "FAN", "FAN2"]
    cols = [UI_GREEN, UI_ORANGE, UI_BLUE, UI_BLUE]
    y = 145 + idx * 16
    bg = UI_SEL if selected else UI_CARD
    s.rrect(9, y - 1, 158, 15, bg)
    s.text(13, y, names[idx], UI_GOLD if selected else UI_MUTED, bg, 1)
    s.text_r(163, y, "%3d%%" % pct, UI_GOLD if selected else UI_TEXT, bg, 1)
    meter_bar(s, 13, y + 9, 150, 4, pct, cols[idx])


def draw_graph(s, hist, sp10):
    bot = PLOT_Y + PLOT_H
    sy = bot - 1 - (sp10 * (PLOT_H - 1)) // 5000
    sy = max(PLOT_Y, min(bot - 1, sy))
    for i in range(PLOT_W):
        px = PLOT_X + i
        py = bot
        if i < len(hist):
            v = max(0, min(600, hist[i]))
            py = bot - 1 - (v * (PLOT_H - 1)) // 600
        if py > PLOT_Y:
            s.fill_rect(px, PLOT_Y, 1, py - PLOT_Y, UI_BG)
        for g in (1, 2, 3):
            gy = PLOT_Y + (PLOT_H * g) // 4
            if gy < py:
                s.fill_rect(px, gy, 1, 1, UI_LINE)
        if (i % 6) < 3 and sy < py:
            s.fill_rect(px, sy, 1, 1, UI_GOLD_D)
        if i < len(hist):
            if py < bot - 1:
                s.fill_rect(px, py + 1, 1, bot - 1 - py, UI_ORANGE_D)
            s.fill_rect(px, py, 1, 1, UI_ORANGE)


def page_home(s, hist, t10, sp10, heat, iron, fan, fan2, st, selrow):
    s.fill_rect(0, 0, W, H, UI_BG)
    draw_header(s, "HEATER CTRL", st)
    s.panel(TC_X, TC_Y, TC_W, TC_H, UI_CARD, UI_EDGE)
    s.text(14, 38, "TEMPERATURE", UI_MUTED, UI_CARD, 1)
    s.fill_rect(139, 52, 4, 4, UI_GOLD)
    s.text(137, 60, "C", UI_GOLD, UI_CARD, 2)
    s.fill_rect(104, 78, 4, 4, UI_GOLD)
    s.panel(6, MC_Y, 164, MC_H, UI_CARD, UI_EDGE)
    s.panel(GRF_X, GRF_Y, GRF_W, GRF_H, UI_CARD, UI_EDGE)
    s.text(184, 38, "TREND", UI_MUTED, UI_CARD, 1)
    s.text_r(308, 38, "60s", UI_MUTED, UI_CARD, 1)
    s.fill_rect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, UI_BG)
    for g in (1, 2, 3):
        gy = PLOT_Y + (PLOT_H * g) // 4
        x = PLOT_X
        while x < PLOT_X + PLOT_W:
            s.fill_rect(x, gy, 2, 1, UI_LINE)
            x += 4
    home_temp(s, t10)
    home_meter(s, 0, heat, False)
    home_meter(s, 1, iron, selrow == 1)
    home_meter(s, 2, fan, selrow == 2)
    home_meter(s, 3, fan2, selrow == 3)
    home_set_block(s, sp10, selrow == 0)
    draw_footer(s, PAGE_HOME, selrow == 4)
    draw_graph(s, hist, sp10)


SET_LABEL = ["HEATER", "FAN MODE", "KP", "KI", "KD"]


def setup_row(s, i, values, selected):
    y = SETUP_TOP + i * SETUP_RH
    bg = UI_SEL if selected else UI_CARD
    s.panel(6, y, 310, 32, bg, UI_GOLD if selected else UI_EDGE)
    s.text(16, y + 5, SET_LABEL[i], UI_GOLD if selected else UI_MUTED, bg, 1)
    s.text(16, y + 15, values[i], UI_GOLD if selected else UI_TEXT, bg, 2)
    if selected:
        s.tri(282, y + 11, 6, 10, UI_GOLD, -1)
        s.tri(298, y + 11, 6, 10, UI_GOLD, +1)


def page_setup(s, values, st, selrow):
    s.fill_rect(0, 0, W, H, UI_BG)
    draw_header(s, "SETTINGS", st)
    s.text_c(160, 203, "UP/DN: ROW    L/R: VALUE", UI_MUTED, UI_BG, 1)
    for i in range(5):
        setup_row(s, i, values, i == selrow)
    draw_footer(s, PAGE_SETUP, selrow == 5)


def page_about(s, uptime, st):
    s.fill_rect(0, 0, W, H, UI_BG)
    draw_header(s, "ABOUT", st)
    trace(s, 6, 58, 34, +1, UI_EDGE)
    trace(s, 6, 76, 22, +1, UI_GOLD_D)
    trace(s, 6, 94, 40, +1, UI_EDGE)
    trace(s, 314, 58, 34, -1, UI_EDGE)
    trace(s, 314, 76, 22, -1, UI_EDGE)
    trace(s, 314, 94, 40, -1, UI_GOLD_D)
    brand(s, 160, 76, 34, UI_GOLD)
    s.text_c(160, 112, "MEHDI GHANBARIAN", UI_GOLD, UI_BG, 2)
    s.text_c(160, 130, "EMBEDDED SYSTEMS ENGINEER", UI_MUTED, UI_BG, 1)
    s.fill_rect(96, 144, 128, 1, UI_EDGE)
    s.text_c(160, 152, "HEATER CONTROLLER  V2.0", UI_TEXT, UI_BG, 1)
    s.text_c(160, 164, "STM32G070  CORTEX-M0+  16MHz", UI_MUTED, UI_BG, 1)
    s.text_c(160, 176, "2CH PHASE DIMMER + PID + FAN", UI_MUTED, UI_BG, 1)
    s.text_c(160, 190, "UPTIME  %02d:%02d:%02d" % uptime, UI_MUTED, UI_BG, 1)
    draw_footer(s, PAGE_ABOUT, False)


# ---------------------------------------------------------------- png
def write_png(path, scr, scale=2):
    w, h = W * scale, H * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = scr.px[(y // scale) * W:(y // scale) * W + W]
        for c in row:
            r = ((c >> 11) & 0x1F) * 255 // 31
            g = ((c >> 5) & 0x3F) * 255 // 63
            b = (c & 0x1F) * 255 // 31
            raw += bytes((r, g, b)) * scale

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    FONT = load_font(os.path.join(here, "..", "Core", "Inc", "font5x7.h"))
    out = os.path.join(here, "..", "Debug")

    # plausible heat-up curve: ambient 25 -> 213.4 over the 63 s window
    hist = []
    for i in range(PLOT_W):
        t = i / (PLOT_W - 1)
        hist.append(int(25 + (213.4 - 25) * (1 - math.exp(-3.1 * t))))

    s = Screen()
    page_home(s, hist, 2134, 2000, 62, 35, 40, 25, 1, 0)
    write_png(os.path.join(out, "ui_home.png"), s)

    s = Screen()
    page_setup(s, ["ON", "AUTO", "2.0", "0.05", "1.0"], 1, 2)
    write_png(os.path.join(out, "ui_setup.png"), s)

    s = Screen()
    page_about(s, (0, 12, 45), 1)
    write_png(os.path.join(out, "ui_about.png"), s)
    print("wrote ui_home.png ui_setup.png ui_about.png")
