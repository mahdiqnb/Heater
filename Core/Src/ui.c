#include "ui.h"
#include "lcd.h"
#include "keys.h"
#include "settings.h"
#include "control.h"
#include "fan.h"
#include "dimmer.h"
#include "logo.h"
#include "stm32g0xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ palette
 * RGB565. The black / gold scheme matches the boot logo so the device looks
 * like one product from the first frame to the last.                        */
#define UI_BG       0x0842  /* #0B0B10 near black            */
#define UI_CARD     0x10A4  /* #171720 card background       */
#define UI_SEL      0x2106  /* #232330 selected row          */
#define UI_EDGE     0x2967  /* #2E2E3A card border           */
#define UI_LINE     0x2124  /* #212124 grid / track          */
#define UI_GOLD     0xE567  /* #E0AC3C primary accent        */
#define UI_GOLD_D   0x8B44  /* #8A6A20 dim accent            */
#define UI_GHOST    0x18C3  /* unlit seven-segment pixels    */
#define UI_TEXT     0xE77E  /* #ECECF2                       */
#define UI_MUTED    0x8C74  /* #8C8CA0 secondary text        */
#define UI_GREEN    0x44AF  /* heater                        */
#define UI_RED      0xEA48  /* fault                         */
#define UI_ORANGE   0xFCC7  /* temperature curve             */
#define UI_ORANGE_D 0x4962  /* curve fill                    */
#define UI_BLUE     0x3D5F  /* fan                           */

/* ------------------------------------------------------------------ layout */
#define HDR_H       28
#define FOOT_Y      212
#define FOOT_H      26

#define TC_X        6
#define TC_Y        32
#define TC_W        164
#define TC_H        107

#define MC_Y        143
#define MC_H        64

#define GRF_X       176
#define GRF_Y       32
#define GRF_W       140
#define GRF_H       175

#define PLOT_X      184
#define PLOT_Y      50
#define PLOT_W      126
#define PLOT_H      151

#define DIG_Y       46
#define DIG_W       22
#define DIG_H       38
#define DIG_TH      6

#define SETUP_TOP   32
#define SETUP_RH    25   /* 7 rows * 25px = 175px; fits before footer at y=212 */
#define SETUP_RH_H  24
#define SETUP_ROWS  7    /* HEATER, FAN MODE, BOX FAN, KP, KI, KD, HEATER % */

#define SPLASH_MS   2000UL
#define SAMPLE_MS   500UL
#define SAVE_IDLE_MS 2000UL

#define PAGE_COUNT  3
#define PAGE_HOME   0
#define PAGE_SETUP  1
#define PAGE_ABOUT  2

/* ------------------------------------------------------------------- state */
static settings_t cfg;

static uint8_t  page = PAGE_HOME;
static uint8_t  sel[PAGE_COUNT];
static uint8_t  sel_dirty;
static uint8_t  heater_on;
static uint8_t  fan_auto;
static uint8_t  fan2_mute;            /* box fan silenced by the user */
static uint8_t  fan2_level;           /* requested box fan duty, 0..50  */
static uint8_t  heater_manual;        /* 0=PID auto, 1=manual % override */
static uint8_t  heater_manual_pct;    /* 0..100, only used when heater_manual=1 */

static void apply_fan2(void);          /* programs the box fan from level + mute */
static int16_t clamp16(int16_t v, int16_t lo, int16_t hi);
static int16_t  sp10 = 2000;          /* setpoint in 0.1 degC */
static int16_t  kp10 = 20, ki100 = 5, kd10 = 10;

static uint8_t  v_heat, v_iron, v_fan, v_fan2;
static uint8_t  cur_status;
static int16_t  v_t10;                /* measured temperature in 0.1 degC */

static char     buf[24];

/* temperature history for the trend graph */
static int16_t  hist[PLOT_W];
static uint8_t  hist_n;
static uint32_t last_sample;
static uint8_t  graph_dirty;
static uint32_t last_graph_ms;        /* throttles the trend redraw */
static uint8_t  keys_busy;            /* a value key is being held */

/* what is already on the glass */
static uint8_t  last_page = 0xFF;
static uint8_t  last_sel_setup = 0xFF;  /* SETUP row currently drawn selected */
static uint8_t  last_status = 0xFF;
static int16_t  last_t10 = -1;
static int16_t  last_sp10 = -1;
static uint8_t  last_heat = 0xFF, last_iron = 0xFF, last_fan = 0xFF, last_fan2 = 0xFF;
static uint8_t  dig_shown[4] = {0xFF, 0xFF, 0xFF, 0xFF};
static uint32_t last_uptime = 0xFFFFFFFFUL;

/* =============================================================== primitives */
static void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    LCD_FillRect(x, y, w, h, c);
}

static void hline(int16_t x, int16_t y, int16_t w, uint16_t c) {
    LCD_FillRect(x, y, w, 1, c);
}

static void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
    int16_t dx = (x1 > x0) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
    /* dy must be negative here: that is what makes the two error tests of the
       Bresenham loop converge. With a positive dy the loop never advances y
       and spins forever -- which is exactly what the status icon did, freezing
       the UI on the first frame. */
    int16_t dy = (y1 > y0) ? (int16_t)(y0 - y1) : (int16_t)(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (int16_t)(dx + dy);
    for (;;) {
        LCD_DrawPixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = (int16_t)(2 * err);
        if (e2 >= dy) { err = (int16_t)(err + dy); x0 = (int16_t)(x0 + sx); }
        if (e2 <= dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
    }
}

/* small filled arrow: dir > 0 points right, dir < 0 points left */
static void tri(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c, int8_t dir) {
    for (int16_t i = 0; i < w; i++) {
        int16_t hh = (int16_t)(1 + (int32_t)(w - 1 - i) * (h - 1) / (w > 1 ? (w - 1) : 1));
        int16_t yy = (int16_t)(y + (h - hh) / 2);
        fill(x + (dir > 0 ? i : (int16_t)(w - 1 - i)), yy, 1, hh, c);
    }
}

/* filled rectangle with softly rounded corners (4px) */
static void rrect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    static const uint8_t ins[4] = {3, 1, 0, 0};
    if (w <= 0 || h <= 0) return;
    if (h < 8 || w < 10) { fill(x, y, w, h, c); return; }
    for (int i = 0; i < 4; i++) {
        int16_t d = (int16_t)ins[i];
        fill((int16_t)(x + d), (int16_t)(y + i), (int16_t)(w - 2 * d), 1, c);
        fill((int16_t)(x + d), (int16_t)(y + h - 1 - i), (int16_t)(w - 2 * d), 1, c);
    }
    fill(x, (int16_t)(y + 4), w, (int16_t)(h - 8), c);
}

static void panel(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg, uint16_t edge) {
    rrect(x, y, w, h, edge);
    rrect((int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 2), (int16_t)(h - 2), bg);
}

static uint16_t shade(uint16_t c, uint8_t pct) {
    uint16_t r = (uint16_t)((c >> 11) & 0x1F);
    uint16_t g = (uint16_t)((c >> 5) & 0x3F);
    uint16_t b = (uint16_t)(c & 0x1F);
    r = (uint16_t)(r * pct / 100);
    g = (uint16_t)(g * pct / 100);
    b = (uint16_t)(b * pct / 100);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void txt(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t sc) {
    LCD_DrawString(x, y, s, fg, bg, sc);
}

static int16_t txt_w(const char *s, uint8_t sc) {
    return LCD_StrWidth(s, sc);
}

static void txt_c(int16_t cx, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t sc) {
    txt((int16_t)(cx - txt_w(s, sc) / 2), y, s, fg, bg, sc);
}

static void txt_r(int16_t rx, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t sc) {
    txt((int16_t)(rx - txt_w(s, sc)), y, s, fg, bg, sc);
}

/* horizontal meter with a small vertical gradient so it does not look flat */
static void meter_bar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t c) {
    fill(x, y, w, h, UI_LINE);
    if (pct > 100) pct = 100;
    int16_t fw = (int16_t)((int32_t)w * pct / 100);
    if (fw <= 0) return;
    for (int16_t i = 0; i < h; i++) {
        uint8_t b = (uint8_t)(100 - (i * 40) / (h > 1 ? (h - 1) : 1));
        fill(x, (int16_t)(y + i), fw, 1, shade(c, b));
    }
}

/* ------------------------------------------------------- seven segment digits */
#define SA 0x01
#define SB 0x02
#define SC 0x04
#define SD 0x08
#define SE 0x10
#define SF 0x20
#define SG 0x40

static const uint8_t DIG_SEG[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void seg_mask(int16_t x, int16_t y, int16_t w, int16_t h, int16_t th,
                     uint8_t m, uint16_t c) {
    int16_t mh = (int16_t)((h - th) / 2);
    int16_t bh = (int16_t)(mh - th);
    if (m & SA) fill((int16_t)(x + th), y, (int16_t)(w - 2 * th), th, c);
    if (m & SG) fill((int16_t)(x + th), (int16_t)(y + (h - th) / 2), (int16_t)(w - 2 * th), th, c);
    if (m & SD) fill((int16_t)(x + th), (int16_t)(y + h - th), (int16_t)(w - 2 * th), th, c);
    if (m & SF) fill(x, (int16_t)(y + th), th, bh, c);
    if (m & SB) fill((int16_t)(x + w - th), (int16_t)(y + th), th, bh, c);
    if (m & SE) fill(x, (int16_t)(y + h - mh), th, bh, c);
    if (m & SC) fill((int16_t)(x + w - th), (int16_t)(y + h - mh), th, bh, c);
}

static void seg_digit(int16_t x, int16_t y, int16_t w, int16_t h, int16_t th,
                      uint8_t d, uint16_t on, uint16_t off) {
    seg_mask(x, y, w, h, th, 0x7F, off);
    seg_mask(x, y, w, h, th, DIG_SEG[d % 10], on);
}

/* ---------------------------------------------------------------- brand mark */
static void brand(int16_t cx, int16_t cy, int16_t r, uint16_t col) {
    int16_t hw = r;
    int16_t hh = (int16_t)(r * 7 / 8);
    line((int16_t)(cx - hw), cy, (int16_t)(cx - hw / 2), (int16_t)(cy - hh), col);
    line((int16_t)(cx - hw / 2), (int16_t)(cy - hh), (int16_t)(cx + hw / 2), (int16_t)(cy - hh), col);
    line((int16_t)(cx + hw / 2), (int16_t)(cy - hh), (int16_t)(cx + hw), cy, col);
    line((int16_t)(cx + hw), cy, (int16_t)(cx + hw / 2), (int16_t)(cy + hh), col);
    line((int16_t)(cx + hw / 2), (int16_t)(cy + hh), (int16_t)(cx - hw / 2), (int16_t)(cy + hh), col);
    line((int16_t)(cx - hw / 2), (int16_t)(cy + hh), (int16_t)(cx - hw), cy, col);
    /* nested chevrons, echoing the logo */
    for (int8_t k = 0; k < 2; k++) {
        int16_t w2 = (int16_t)(r * 3 / 5 - k * (r / 4));
        int16_t vy = (int16_t)(cy - r / 6 + k * (r / 3));
        line((int16_t)(cx - w2), (int16_t)(vy - r / 4), cx, vy, col);
        line(cx, vy, (int16_t)(cx + w2), (int16_t)(vy - r / 4), col);
    }
}

/* decorative pcb trace with a pad on the end */
static void trace(int16_t x, int16_t y, int16_t len, int8_t dir, uint16_t c) {
    int16_t ex = (int16_t)(x + dir * len);
    hline(x < ex ? x : ex, y, len, c);
    rrect((int16_t)(ex - 2), (int16_t)(y - 2), 5, 5, c);
}

/* =============================================================== chrome */
static void draw_status(uint8_t st) {
    static const char *lbl[6] = {"STOP", "RUN ", "OVER", "IDLE", "NO HEAT",
                                  "NO AC"};
    if (st > 5) st = 0;
    uint16_t col = (st == 2 || st == 4 || st == 5) ? UI_RED
                 : (st == 1) ? UI_GREEN : (st == 3) ? UI_GOLD_D : UI_MUTED;
    rrect(238, 7, 74, 16, col);
    rrect(239, 8, 72, 14, UI_CARD);
    txt_c(275, 12, lbl[st], col, UI_CARD, 1);
}

static void draw_header(const char *title) {
    fill(0, 0, LCD_W, HDR_H, UI_CARD);
    hline(0, HDR_H - 2, LCD_W, UI_EDGE);
    fill(0, HDR_H - 2, 46, 2, UI_GOLD);
    brand(18, 14, 9, UI_GOLD);
    txt(34, 7, title, UI_GOLD, UI_CARD, 2);
    txt(178, 11, "V2.0", UI_MUTED, UI_CARD, 1);
    draw_status(cur_status);
}

/* the footer doubles as the page selector: it is one of the selectable rows */
static void draw_footer(uint8_t selected) {
    static const char *names[PAGE_COUNT] = {"HOME", "SETTINGS", "ABOUT"};
    uint16_t bg = selected ? UI_SEL : UI_CARD;
    uint16_t edge = selected ? UI_GOLD : UI_EDGE;
    rrect(6, FOOT_Y, 308, FOOT_H, edge);
    rrect(7, (int16_t)(FOOT_Y + 1), 306, (int16_t)(FOOT_H - 2), bg);
    tri(16, (int16_t)(FOOT_Y + 8), 6, 10, UI_GOLD_D, +1);
    tri(298, (int16_t)(FOOT_Y + 8), 6, 10, UI_GOLD_D, -1);
    txt_c(160, (int16_t)(FOOT_Y + 6), names[page], selected ? UI_GOLD : UI_TEXT, bg, 2);
    for (int8_t i = 0; i < PAGE_COUNT; i++) {
        fill((int16_t)(152 + i * 8), (int16_t)(FOOT_Y + 20), 3, 3,
             (i == (int8_t)page) ? UI_GOLD : UI_EDGE);
    }
}

/* =============================================================== home page */
static void home_digit(uint8_t slot, uint8_t val, uint8_t blank) {
    static const int16_t xs[4] = {28, 54, 80, 110};
    uint8_t sig = blank ? 0xFE : val;
    if (dig_shown[slot] == sig) return;
    dig_shown[slot] = sig;
    if (blank) {
        fill(xs[slot], DIG_Y, DIG_W, DIG_H, UI_CARD);
        return;
    }
    seg_digit(xs[slot], DIG_Y, DIG_W, DIG_H, DIG_TH, val, UI_GOLD, UI_GHOST);
}

static void home_temp(int16_t t10) {
    if (t10 < 0) t10 = 0;
    if (t10 > 9999) t10 = 9999;
    home_digit(0, (uint8_t)((t10 / 1000) % 10), t10 < 1000);
    home_digit(1, (uint8_t)((t10 / 100) % 10), t10 < 100);
    home_digit(2, (uint8_t)((t10 / 10) % 10), t10 < 10);
    home_digit(3, (uint8_t)(t10 % 10), 0);
}

static void home_set_block(uint8_t selected) {
    uint16_t bg = selected ? UI_SEL : UI_CARD;
    rrect(9, 99, 158, 29, selected ? UI_GOLD : UI_EDGE);
    rrect(10, 100, 156, 27, bg);
    txt(16, 104, "SET", selected ? UI_GOLD_D : UI_MUTED, bg, 1);
    /* 25.0 C and below is not a temperature, it is "off" */
    if (sp10 <= HEATER_MIN_SP_X10)
        strcpy(buf, "OFF");
    else
        sprintf(buf, "%d.%d C", sp10 / 10, sp10 % 10);
    txt_r(158, 107, buf, (sp10 <= HEATER_MIN_SP_X10) ? UI_RED
        : (selected ? UI_GOLD : UI_TEXT), bg, 2);
}

static void home_meter(uint8_t idx, uint8_t pct, uint8_t selected) {
    static const char *names[4] = {"HEAT", "IRON", "FAN", "FAN2"};
    static const uint16_t cols[4] = {UI_GREEN, UI_ORANGE, UI_BLUE, UI_BLUE};
    int16_t y = (int16_t)(145 + (int16_t)idx * 16);
    uint16_t bg = selected ? UI_SEL : UI_CARD;
    /* the box fan row is also the mute indicator: a silenced fan must not be
       mistaken for a fan that is simply set to 0 % */
    uint8_t muted = (idx == 3) && fan2_mute;
    rrect(9, (int16_t)(y - 1), 158, 15, bg);
    txt(13, y, names[idx], selected ? UI_GOLD : UI_MUTED, bg, 1);
    if (muted) {
        strcpy(buf, "MUTE");
        txt_r(163, y, buf, UI_RED, bg, 1);
    } else {
        sprintf(buf, "%3d%%", pct);
        txt_r(163, y, buf, selected ? UI_GOLD : UI_TEXT, bg, 1);
    }
    meter_bar(13, (int16_t)(y + 9), 150, 4, muted ? 0 : pct, cols[idx]);
}

static void draw_graph(void) {
    int16_t bot = (int16_t)(PLOT_Y + PLOT_H);
    /* setpoint marker line */
    int32_t sy32 = (int32_t)bot - 1 - ((int32_t)sp10 * (PLOT_H - 1)) / 5000;
    int16_t sy = (int16_t)sy32;
    if (sy < PLOT_Y) sy = PLOT_Y;
    if (sy > bot - 1) sy = bot - 1;

    for (int16_t i = 0; i < PLOT_W; i++) {
        int16_t px = (int16_t)(PLOT_X + i);
        int16_t py = bot;               /* nothing in this column yet */
        if (i < (int16_t)hist_n) {
            int32_t v = hist[i];
            if (v < 0) v = 0;
            if (v > 600) v = 600;
            py = (int16_t)(bot - 1 - (v * (PLOT_H - 1)) / 600);
        }
        if (py > PLOT_Y) fill(px, PLOT_Y, 1, (int16_t)(py - PLOT_Y), UI_BG);
        /* re-draw the grid dots this column just cleared */
        for (int8_t g = 1; g <= 3; g++) {
            int16_t gy = (int16_t)(PLOT_Y + (PLOT_H * g) / 4);
            if (gy < py) fill(px, gy, 1, 1, UI_LINE);
        }
        if (((i % 6) < 3) && sy < py) fill(px, sy, 1, 1, UI_GOLD_D);
        if (i < (int16_t)hist_n) {
            if (py < bot - 1) fill(px, (int16_t)(py + 1), 1, (int16_t)(bot - 1 - py), UI_ORANGE_D);
            fill(px, py, 1, 1, UI_ORANGE);
        }
    }
}

static void draw_home_static(void) {
    uint8_t s = sel[PAGE_HOME];
    fill(0, 0, LCD_W, LCD_H, UI_BG);
    draw_header("HEATER CTRL");

    panel(TC_X, TC_Y, TC_W, TC_H, UI_CARD, UI_EDGE);
    txt(14, 38, "TEMPERATURE", UI_MUTED, UI_CARD, 1);
    fill(139, 52, 4, 4, UI_GOLD);      /* degree dot */
    txt(137, 60, "C", UI_GOLD, UI_CARD, 2);
    fill(104, 78, 4, 4, UI_GOLD);      /* decimal point */

    panel(6, MC_Y, 164, MC_H, UI_CARD, UI_EDGE);

    panel(GRF_X, GRF_Y, GRF_W, GRF_H, UI_CARD, UI_EDGE);
    txt(184, 38, "TREND", UI_MUTED, UI_CARD, 1);
    txt_r(308, 38, "60s", UI_MUTED, UI_CARD, 1);
    fill(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, UI_BG);
    for (int8_t g = 1; g <= 3; g++) {
        int16_t gy = (int16_t)(PLOT_Y + (PLOT_H * g) / 4);
        for (int16_t x = PLOT_X; x < PLOT_X + PLOT_W; x += 4) fill(x, gy, 2, 1, UI_LINE);
    }

    /* rows */
    home_meter(0, v_heat, 0);          /* HEAT is read-only */
    home_meter(1, v_iron, s == 1);
    home_meter(2, v_fan, s == 2);
    home_meter(3, v_fan2, s == 3);
    home_set_block(s == 0);
    draw_footer(s == 4);
    dig_shown[0] = dig_shown[1] = dig_shown[2] = dig_shown[3] = 0xFF;
    draw_graph();
}

/* ============================================================== setup page */
static const char *SET_LABEL[SETUP_ROWS] = {
    "HEATER", "FAN MODE", "BOX FAN", "KP", "KI", "KD", "HEATER %"
};

static void setup_row(uint8_t i, uint8_t selected) {
    int16_t y = (int16_t)(SETUP_TOP + i * SETUP_RH);
    uint16_t bg = selected ? UI_SEL : UI_CARD;
    panel(6, y, 310, SETUP_RH_H, bg, selected ? UI_GOLD : UI_EDGE);
    txt(16, (int16_t)(y + 4), SET_LABEL[i], selected ? UI_GOLD : UI_MUTED, bg, 1);

    switch (i) {
    case 0: strcpy(buf, heater_on ? "ON" : "OFF"); break;
    case 1: strcpy(buf, fan_auto ? "AUTO" : "MANUAL"); break;
    case 2: strcpy(buf, fan2_mute ? "MUTE" : "ON"); break;
    case 3: sprintf(buf, "%d.%d", kp10 / 10, kp10 % 10); break;
    case 4: sprintf(buf, "%d.%02d", ki100 / 100, ki100 % 100); break;
    case 5: sprintf(buf, "%d.%d", kd10 / 10, kd10 % 10); break;
    default:
        /* HEATER %: show mode + value. MANUAL overrides PID output. */
        if (heater_manual)
            sprintf(buf, "MAN %d%%", heater_manual_pct);
        else
            strcpy(buf, "PID AUTO");
        break;
    }
    txt(16, (int16_t)(y + 11), buf,
        (i == 2 && fan2_mute) ? UI_RED :
        (i == 6 && heater_manual) ? UI_ORANGE :
        (selected ? UI_GOLD : UI_TEXT), bg, 2);

    if (selected) {
        tri(282, (int16_t)(y + 8), 6, 10, UI_GOLD, -1);
        tri(298, (int16_t)(y + 8), 6, 10, UI_GOLD, +1);
    }
}

static void draw_setup_static(void) {
    fill(0, 0, LCD_W, LCD_H, UI_BG);
    draw_header("SETTINGS");
    txt_c(160, 203, "UP/DN: ROW    L/R: VALUE", UI_MUTED, UI_BG, 1);
}

/* ============================================================== about page */
static void draw_about(void) {
    fill(0, 0, LCD_W, LCD_H, UI_BG);
    draw_header("ABOUT");

    trace(6, 58, 34, +1, UI_EDGE);
    trace(6, 76, 22, +1, UI_GOLD_D);
    trace(6, 94, 40, +1, UI_EDGE);
    trace(314, 58, 34, -1, UI_EDGE);
    trace(314, 76, 22, -1, UI_EDGE);
    trace(314, 94, 40, -1, UI_GOLD_D);

    brand(160, 76, 34, UI_GOLD);

    txt_c(160, 112, "MEHDI GHANBARIAN", UI_GOLD, UI_BG, 2);
    txt_c(160, 130, "EMBEDDED SYSTEMS ENGINEER", UI_MUTED, UI_BG, 1);
    hline(96, 144, 128, UI_EDGE);
    txt_c(160, 152, "HEATER CONTROLLER  V2.0", UI_TEXT, UI_BG, 1);
    txt_c(160, 164, "STM32G070  CORTEX-M0+  16MHz", UI_MUTED, UI_BG, 1);
    txt_c(160, 176, "2CH PHASE DIMMER + PID + FAN", UI_MUTED, UI_BG, 1);
    last_uptime = 0xFFFFFFFFUL;
    draw_footer(sel[PAGE_ABOUT] == 0);
}

/* =========================================================== page mechanics */
static void render_page(void) {
    switch (page) {
    case PAGE_HOME:
        draw_home_static();
        home_temp(v_t10);
        break;
    case PAGE_SETUP:
        draw_setup_static();
        for (uint8_t i = 0; i < SETUP_ROWS; i++) setup_row(i, sel[PAGE_SETUP] == i);
        draw_footer(sel[PAGE_SETUP] == SETUP_ROWS);
        break;
    default:
        draw_about();
        break;
    }
}

static void sync_cache(void) {
    last_page = page;
    last_sel_setup = sel[PAGE_SETUP];
    last_status = cur_status;
    last_t10 = v_t10;
    last_sp10 = sp10;
    last_heat = v_heat;
    last_iron = v_iron;
    last_fan = v_fan;
    last_fan2 = v_fan2;
    dig_shown[0] = dig_shown[1] = dig_shown[2] = dig_shown[3] = 0xFF;
    graph_dirty = 0;
    sel_dirty = 0;
}

/* ================================================================== splash */
void UI_Splash(void) {
    LCD_Fill(0x0000);
    LCD_DrawIndexed(LOGO_X, LOGO_Y, LOGO_W, LOGO_H, logo_pix, logo_pal);

    uint32_t t0 = HAL_GetTick();
    int16_t last_w = -1;
    fill(10, 232, 300, 3, UI_EDGE);
    while ((HAL_GetTick() - t0) < SPLASH_MS) {
        Keys_Update();
        uint8_t skip = Key_Up_Pressed() | Key_Down_Pressed() |
                       Key_Left_Pressed() | Key_Right_Pressed();
        if (skip) break;
        uint32_t el = HAL_GetTick() - t0;
        int16_t w = (int16_t)((el * 300UL) / SPLASH_MS);
        if ((w / 6) != (last_w / 6)) {
            if (w > last_w) fill(10, 232, w, 3, UI_GOLD);
            last_w = w;
        }
        HAL_Delay(5);
    }
}

/* ==================================================================== init */
void UI_Init(void) {
    Settings_Load(&cfg);
    sp10 = cfg.setpoint_x10;
    /* older records could hold anything; 25.0 C is the OFF floor */
    if (sp10 < HEATER_MIN_SP_X10) sp10 = HEATER_MIN_SP_X10;
    heater_on = cfg.enabled ? 1 : 0;
    fan_auto = cfg.fan_auto ? 1 : 0;
    kp10 = cfg.kp_x10;
    ki100 = cfg.ki_x100;
    kd10 = cfg.kd_x10;

    fan2_mute = cfg.fan2_mute ? 1 : 0;
    fan2_level = (uint8_t)clamp16(cfg.fan2_pct, 0, FAN_CH1_MAX_PCT);

    Dimmer_SetIron(cfg.iron_pct);
    Fan_SetSpeed(FAN_CH0, cfg.fan_pct);
    apply_fan2();
    Fan_SetAuto(fan_auto);
    Control_SetGains((float)kp10 / 10.0f, (float)ki100 / 100.0f, (float)kd10 / 10.0f);
    Control_SetEnabled(heater_on);

    memset(hist, 0, sizeof(hist));
    hist_n = 0;
    last_sample = HAL_GetTick();
    graph_dirty = 1;

    /* a key pressed to skip the splash is already consumed as an edge turn;
       drop the "physical press" flag too so the first pass does not act on
       it (it would otherwise flip a switch behind the user's back) */
    (void)Key_Left_New();
    (void)Key_Right_New();

    page = PAGE_HOME;
    sel[PAGE_HOME] = sel[PAGE_SETUP] = sel[PAGE_ABOUT] = 0;
    sel_dirty = 1;

    v_heat = 0;
    v_iron = Dimmer_GetIron();
    v_fan = Fan_GetSpeed(FAN_CH0);
    v_fan2 = Fan_GetSpeed(FAN_CH1);
    cur_status = heater_on ? 1 : 0;

    v_t10 = 0;
    last_t10 = 0;
    last_status = 0xFF;
    render_page();
    sync_cache();
}

float UI_GetSetpoint(void) { return (float)sp10 / 10.0f; }
uint8_t UI_GetIron(void) { return Dimmer_GetIron(); }
uint8_t UI_GetFan(void) { return Fan_GetSpeed(FAN_CH0); }
uint8_t UI_GetFan2(void) { return Fan_GetSpeed(FAN_CH1); }
uint8_t UI_GetEnabled(void) { return heater_on; }
/* Returns 1 if user has switched heater to manual % mode */
uint8_t UI_GetHeaterManual(void) { return heater_manual; }
/* Returns the manual heater duty (0..100), valid only when UI_GetHeaterManual()==1 */
uint8_t UI_GetHeaterManualPct(void) { return heater_manual_pct; }

/* ------------------------------------------------------------------ saving */
static uint8_t  settings_dirty;
static uint32_t settings_dirty_at;

static void save_cfg(void) {
    cfg.setpoint_x10 = sp10;
    cfg.iron_pct = Dimmer_GetIron();
    cfg.fan_pct = Fan_GetSpeed(FAN_CH0);
    cfg.fan2_pct = fan2_level;
    cfg.fan2_mute = fan2_mute;
    cfg.enabled = heater_on;
    cfg.fan_auto = fan_auto;
    cfg.kp_x10 = kp10;
    cfg.ki_x100 = ki100;
    cfg.kd_x10 = kd10;
    Settings_Save(&cfg);
}

static void mark_dirty(void) {
    settings_dirty = 1;
    settings_dirty_at = HAL_GetTick();
}

/* ==================================================================== keys */
static int16_t clamp16(int16_t v, int16_t lo, int16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* the box fan runs from the stored level, gated by the mute switch, so
   unmuting always gives back the level the user asked for */
static void apply_fan2(void) {
    Fan_SetSpeed(FAN_CH1, fan2_mute ? 0 : fan2_level);
}

void UI_HandleKeys(void) {
    Keys_Update();
    uint8_t up = Key_Up_Pressed();
    uint8_t dn = Key_Down_Pressed();
    uint8_t lf = Key_Left_Pressed();
    uint8_t rt = Key_Right_Pressed();

    /* the physical press only: switches and page changes must fire once per
       press, never at the auto-repeat rate */
    uint8_t lf_new = Key_Left_New();
    uint8_t rt_new = Key_Right_New();
    uint8_t new_press = (lf_new || rt_new) ? 1 : 0;

    /* The longer the key is held, the bigger the step: x1, then x2 after
       0.6 s, x5 after 1.5 s and x10 after 3 s (keys.c reports the hold time).
       Starting the ramp early is what makes 200 C -> 400 C take a second or
       two instead of 200 taps -- waiting for one single 3 s threshold felt
       like nothing was happening. */
    uint32_t hold = 0;
    if (lf) hold = Key_Left_HoldMs();
    if (rt) {
        uint32_t h = Key_Right_HoldMs();
        if (h > hold) hold = h;
    }
    uint8_t stage = (hold >= 3000u) ? 3u : (hold >= 1500u) ? 2u
                  : (hold >= 600u) ? 1u : 0u;
    static const int16_t TEMP_STEP[4] = {1, 2, 5, 10};   /* degC per step   */
    static const int16_t PCT_STEP[4]  = {5, 5, 15, 25};  /* percent         */
    static const int16_t GAIN_STEP[4] = {1, 1, 2, 5};    /* 0.1 / 0.01 unit */

    /* a held key defers the expensive trend redraw, see UI_Update */
    keys_busy = (up || dn || lf || rt) ? 1 : 0;

    if (up || dn || lf || rt) {
        uint8_t n = (page == PAGE_HOME) ? 5
                  : (page == PAGE_SETUP) ? (SETUP_ROWS + 1) : 1;
        uint8_t s = sel[page];

        if (up) { s = (s == 0) ? (uint8_t)(n - 1) : (uint8_t)(s - 1); sel_dirty = 1; }
        if (dn) { s = (uint8_t)((s + 1) % n); sel_dirty = 1; }
        sel[page] = s;

        /* Only process L/R if no row-navigation key was also active this pass.
         * A simultaneous up+lf (e.g. from bounce) would otherwise both move
         * the cursor AND change the value, causing the double-step the user
         * reported in the SETTINGS menu. */
        if ((lf || rt) && !up && !dn) {
            int8_t dir = rt ? 1 : -1;
            if (page == PAGE_HOME) {
                if (s == 0) {
                    /* setpoint: 1 C per step, 5 C per step once the key is
                       held past the fast threshold. The floor is
                       HEATER_MIN_SP_X10: at 25.0 C the heater is OFF, so the
                       value must never fall below it. */
                    sp10 = clamp16((int16_t)(sp10 + dir * 10 * TEMP_STEP[stage]), HEATER_MIN_SP_X10, 5000);
                    mark_dirty();
                } else if (s == 1) {
                    Dimmer_SetIron((uint8_t)clamp16((int16_t)((int16_t)Dimmer_GetIron() + dir * PCT_STEP[stage]), 0, 100));
                    mark_dirty();
                } else if (s == 2) {
                    Fan_SetSpeed(FAN_CH0,
                                 (uint8_t)clamp16((int16_t)((int16_t)Fan_GetSpeed(FAN_CH0) + dir * PCT_STEP[stage]), 0, 100));
                    Fan_SetAuto(0);
                    fan_auto = 0;
                    mark_dirty();
                } else if (s == 3) {
                    /* the 12 V fan on XH501 is clamped by fan.c, keep the UI
                       inside the same window so the bar never lies */
                    fan2_level = (uint8_t)clamp16((int16_t)((int16_t)fan2_level + dir * PCT_STEP[stage]), 0, FAN_CH1_MAX_PCT);
                    apply_fan2();
                    mark_dirty();
                } else if (new_press) {
                    /* page flip: once per press, not at the repeat rate */
                    page = (uint8_t)((page + dir + PAGE_COUNT) % PAGE_COUNT);
                }
            } else if (page == PAGE_SETUP) {
                /* repainting one row instead of all six keeps the auto-repeat
                   responsive: a full repaint over bit-banged SPI is slow */
                uint8_t one_row = 1;
                switch (s) {
                case 0:
                    /* switches act on the physical press only */
                    if (!new_press) { one_row = 0; break; }
                    heater_on = heater_on ? 0 : 1;
                    /* switching on by hand is also the acknowledge of a
                       sensor fault: the user says "try again" */
                    if (heater_on) Control_ClearFault();
                    Control_SetEnabled(heater_on);
                    mark_dirty();
                    break;
                case 1:
                    if (!new_press) { one_row = 0; break; }
                    fan_auto = fan_auto ? 0 : 1;
                    Fan_SetAuto(fan_auto);
                    mark_dirty();
                    break;
                case 2:
                    /* the silent switch: left/right toggles the box fan */
                    if (!new_press) { one_row = 0; break; }
                    fan2_mute = fan2_mute ? 0 : 1;
                    apply_fan2();
                    mark_dirty();
                    break;
                case 3:
                    kp10 = clamp16((int16_t)(kp10 + dir * GAIN_STEP[stage]), 0, 200);
                    Control_SetGains((float)kp10 / 10.0f, (float)ki100 / 100.0f, (float)kd10 / 10.0f);
                    mark_dirty();
                    break;
                case 4:
                    ki100 = clamp16((int16_t)(ki100 + dir * GAIN_STEP[stage]), 0, 500);
                    Control_SetGains((float)kp10 / 10.0f, (float)ki100 / 100.0f, (float)kd10 / 10.0f);
                    mark_dirty();
                    break;
                case 5:
                    kd10 = clamp16((int16_t)(kd10 + dir * GAIN_STEP[stage]), 0, 200);
                    Control_SetGains((float)kp10 / 10.0f, (float)ki100 / 100.0f, (float)kd10 / 10.0f);
                    mark_dirty();
                    break;
                case 6:
                    /* HEATER %: L/R toggles between PID-AUTO and manual.
                     * In auto: first press switches to manual (new_press only).
                     * In manual: L/R adjusts duty; a second new_press returns
                     * to auto and hands control back to the PID. */
                    if (!heater_manual) {
                        if (!new_press) { one_row = 0; break; }
                        heater_manual = 1;
                        heater_manual_pct = (v_heat > 0) ? v_heat : 10;
                        Control_SetEnabled(0);  /* PID off, dimmer driven directly */
                        Dimmer_SetHeater(heater_manual_pct);
                    } else {
                        /* already in manual: if it's the very first press
                         * AND the key went in the "exit" direction, treat it
                         * as a toggle-back so the user can escape with one tap. */
                        static uint8_t man_press_count = 0;
                        if (new_press) man_press_count++;
                        if (man_press_count >= 1 &&
                            (heater_manual_pct == 0) && dir < 0 && new_press) {
                            /* already at 0 and pressing left → exit manual */
                            heater_manual = 0;
                            man_press_count = 0;
                            Control_SetEnabled(heater_on);
                            Dimmer_SetHeater(0);
                        } else {
                            int16_t np = (int16_t)heater_manual_pct + dir * (int16_t)PCT_STEP[stage];
                            if (np < 0) {
                                /* hit the floor → exit manual mode */
                                heater_manual = 0;
                                man_press_count = 0;
                                Control_SetEnabled(heater_on);
                                Dimmer_SetHeater(0);
                            } else {
                                heater_manual_pct = (uint8_t)clamp16(np, 0, 95);
                                Dimmer_SetHeater(heater_manual_pct);
                            }
                        }
                    }
                    break;
                default:
                    one_row = 0;
                    if (!new_press) break;
                    page = (uint8_t)((page + dir + PAGE_COUNT) % PAGE_COUNT);
                    sel_dirty = 1;
                    break;
                }
                if (one_row) setup_row(s, 1);
                else sel_dirty = 1;
            } else {
                /* PAGE_ABOUT: only one action = page flip; use new_press to
                   avoid repeating the flip while the key is held */
                if (new_press) {
                    page = (uint8_t)((page + dir + PAGE_COUNT) % PAGE_COUNT);
                    sel_dirty = 1;
                }
            }
        }
    }

    if (settings_dirty && (HAL_GetTick() - settings_dirty_at) >= SAVE_IDLE_MS) {
        settings_dirty = 0;
        save_cfg();
    }
}

/* ================================================================== update */
void UI_Update(float measured_c, uint8_t heater_pct, uint8_t iron_pct,
               uint8_t fan_pct, uint8_t fan2_pct, uint8_t fault) {
    uint32_t now = HAL_GetTick();

    v_heat = heater_pct;
    v_iron = iron_pct;
    v_fan = fan_pct;
    v_fan2 = fan2_pct;
    v_t10 = (int16_t)(measured_c * 10.0f + 0.5f);
    if (v_t10 < 0) v_t10 = 0;
    /* In manual mode the element is driven even when the HEATER switch is
       off (the user asked for a fixed duty directly), so the header must say
       RUN instead of STOP. */
    cur_status = (fault == 2) ? 4 : (fault == 1) ? 2
               : (heater_manual ? 1
               : (!heater_on ? 0
               : (sp10 <= HEATER_MIN_SP_X10 ? 3 : 1)));
    /* a channel is asking for power but no mains edge arrives: the triacs can
       never be fired, so show it instead of silently doing nothing */
    if ((v_heat > 0 || v_iron > 0) && !Dimmer_ZcOk()) cur_status = 5;

    /* the trend keeps filling while a menu page is open */
    if ((now - last_sample) >= SAMPLE_MS) {
        last_sample = now;
        int16_t v = (int16_t)(measured_c + 0.5f);
        if (v < 0) v = 0;
        if (v > 600) v = 600;
        if (hist_n < PLOT_W) {
            hist[hist_n++] = v;
        } else {
            memmove(hist, hist + 1, sizeof(hist) - sizeof(hist[0]));
            hist[PLOT_W - 1] = v;
        }
        graph_dirty = 1;
    }

    if (page != last_page) {
        render_page();
        sync_cache();
        return;
    }

    if (cur_status != last_status) {
        last_status = cur_status;
        draw_status(cur_status);
    }

    if (page == PAGE_HOME) {
        uint8_t s = sel[PAGE_HOME];

        if (sel_dirty) {
            home_meter(0, v_heat, 0);
            home_meter(1, v_iron, s == 1);
            home_meter(2, v_fan, s == 2);
            home_meter(3, v_fan2, s == 3);
            home_set_block(s == 0);
            draw_footer(s == 4);
            sel_dirty = 0;
        }
        home_temp(v_t10);
        last_t10 = v_t10;

        if (v_heat != last_heat) { last_heat = v_heat; home_meter(0, v_heat, 0); }
        if (v_iron != last_iron) { last_iron = v_iron; home_meter(1, v_iron, s == 1); }
        if (v_fan != last_fan)  { last_fan = v_fan;  home_meter(2, v_fan, s == 2); }
        if (v_fan2 != last_fan2) { last_fan2 = v_fan2; home_meter(3, v_fan2, s == 3); }
        if (sp10 != last_sp10) { last_sp10 = sp10; home_set_block(s == 0); graph_dirty = 1; }

        /* One trend redraw per key-repeat tick would cost more than the tick
           itself over the SPI display, so while a value key is held the graph
           is refreshed at most every 150 ms; releasing the key repaints it
           immediately. */
        if (graph_dirty && (!keys_busy || (now - last_graph_ms) >= 150UL)) {
            graph_dirty = 0;
            last_graph_ms = now;
            draw_graph();
        }
    } else if (page == PAGE_SETUP) {
        if (sel_dirty) {
            uint8_t cur = sel[PAGE_SETUP];
            uint8_t prev = last_sel_setup;

            if (prev > SETUP_ROWS) {
                /* no valid cached row (first paint): redraw everything */
                for (uint8_t i = 0; i < SETUP_ROWS; i++)
                    setup_row(i, cur == i);
                draw_footer(cur == SETUP_ROWS);
            } else {
                /* Only the two rows whose "selected" look changed need a
                 * repaint. A full seven-row repaint over the bit-banged SPI
                 * stalls the main loop for hundreds of milliseconds, which is
                 * what made the cursor feel like it jumped two rows: the
                 * second press arrived while the first repaint was still
                 * running. */
                if (prev < SETUP_ROWS && prev != cur)
                    setup_row(prev, 0);
                if (cur < SETUP_ROWS)
                    setup_row(cur, 1);
                if ((prev == SETUP_ROWS) != (cur == SETUP_ROWS))
                    draw_footer(cur == SETUP_ROWS);
            }
            last_sel_setup = cur;
            sel_dirty = 0;
        }
    } else {
        if (sel_dirty) {
            draw_footer(sel[PAGE_ABOUT] == 0);
            sel_dirty = 0;
        }
        uint32_t secs = now / 1000UL;
        if (secs != last_uptime) {
            last_uptime = secs;
            sprintf(buf, "UPTIME  %02lu:%02lu:%02lu",
                    (unsigned long)(secs / 3600UL),
                    (unsigned long)((secs / 60UL) % 60UL),
                    (unsigned long)(secs % 60UL));
            txt_c(160, 190, buf, UI_MUTED, UI_BG, 1);
        }
    }
}
