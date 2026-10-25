#include "ui.h"
#include "lcd.h"
#include "keys.h"
#include "settings.h"
#include "control.h"
#include "fan.h"
#include "dimmer.h"
#include "temp.h"
#include "stm32g0xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ---------- layout ---------- */
#define HDR_H      22
#define LEFT_W     168
#define GRAPH_X    174
#define GRAPH_Y    30
#define GRAPH_W    142
#define GRAPH_H    176
#define T_MIN      0.0f
#define T_MAX      500.0f

static settings_t cfg;
static int cursor = 0; /* 0=setpoint, 1=iron, 2=fan */
static float setpoint = 200.0f;
static char buf[64];

/* temperature history for the trend graph */
static int16_t hist[GRAPH_W];
static uint8_t hist_count = 0;
static uint32_t last_sample = 0;

void UI_Init(void) {
    Settings_Load(&cfg);
    setpoint = cfg.setpoint_x10 / 10.0f;
    Dimmer_SetIron(cfg.iron_pct);
    Fan_SetSpeed(cfg.fan_pct);
    Control_SetEnabled(cfg.enabled);
    memset(hist, 0, sizeof(hist));
    hist_count = 0;
    LCD_Fill(C_BLACK);
}

float UI_GetSetpoint(void) { return setpoint; }
uint8_t UI_GetIron(void) { return Dimmer_GetIron(); }
uint8_t UI_GetFan(void) { return Fan_GetSpeed(); }
uint8_t UI_GetEnabled(void) { return Control_GetEnabled(); }

static void save_cfg(void) {
    cfg.setpoint_x10 = (int16_t)(setpoint * 10);
    cfg.iron_pct = Dimmer_GetIron();
    cfg.fan_pct = Fan_GetSpeed();
    cfg.enabled = Control_GetEnabled();
    cfg.magic = 0x48454154UL;
    Settings_Save(&cfg);
}

/* ---------- graphical helpers ---------- */
static void draw_bar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t color) {
    /* border */
    LCD_FillRect(x, y, w, h, C_GRAY);
    LCD_FillRect(x + 1, y + 1, w - 2, h - 2, C_BLACK);
    int16_t fill = (int16_t)((uint32_t)(w - 2) * pct / 100);
    if (fill > 0) LCD_FillRect(x + 1, y + 1, fill, h - 2, color);
}

static void draw_graph(void) {
    /* frame */
    LCD_FillRect(GRAPH_X - 2, GRAPH_Y - 2, GRAPH_W + 4, GRAPH_H + 4, C_GRAY);
    LCD_FillRect(GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, C_BLACK);

    /* setpoint line (dashed yellow) */
    int sy = GRAPH_Y + GRAPH_H - 1 - (int)((setpoint - T_MIN) * (GRAPH_H - 1) / (T_MAX - T_MIN));
    if (sy < GRAPH_Y) sy = GRAPH_Y;
    if (sy > GRAPH_Y + GRAPH_H - 1) sy = GRAPH_Y + GRAPH_H - 1;
    for (int x = 0; x < GRAPH_W; x += 4) {
        LCD_FillRect(GRAPH_X + x, sy, 2, 1, C_YELLOW);
    }

    /* temperature curve */
    for (int i = 0; i < hist_count && i < GRAPH_W; i++) {
        int v = hist[i];
        if (v < 0) v = 0;
        if (v > 600) v = 600;
        int py = GRAPH_Y + GRAPH_H - 1 - (int)((int32_t)v * (GRAPH_H - 1) / (int)(T_MAX - T_MIN));
        if (py < GRAPH_Y) py = GRAPH_Y;
        if (py > GRAPH_Y + GRAPH_H - 1) py = GRAPH_Y + GRAPH_H - 1;
        LCD_FillRect(GRAPH_X + i, py, 1, GRAPH_Y + GRAPH_H - py, C_CYAN);
    }
}

/* ---------- keys ---------- */
void UI_HandleKeys(void) {
    Keys_Update();
    uint8_t changed = 0;
    if (Key_Up_Pressed()) {
        if (cursor == 0) { setpoint += 1; if (setpoint > 500) setpoint = 500; changed = 1; }
        else if (cursor == 1) { uint8_t v = Dimmer_GetIron(); if (v < 100) { Dimmer_SetIron(v + 1); changed = 1; } }
        else { uint8_t v = Fan_GetSpeed(); if (v < 100) { Fan_SetSpeed(v + 1); Fan_SetAuto(0); changed = 1; } }
    }
    if (Key_Down_Pressed()) {
        if (cursor == 0) { setpoint -= 1; if (setpoint < 0) setpoint = 0; changed = 1; }
        else if (cursor == 1) { uint8_t v = Dimmer_GetIron(); if (v > 0) { Dimmer_SetIron(v - 1); changed = 1; } }
        else { uint8_t v = Fan_GetSpeed(); if (v > 0) { Fan_SetSpeed(v - 1); Fan_SetAuto(0); changed = 1; } }
    }
    if (Key_Left_Pressed()) { cursor--; if (cursor < 0) cursor = 2; }
    if (Key_Right_Pressed()) { cursor++; if (cursor > 2) cursor = 0; }
    if (changed) save_cfg();
}

/* ---------- main draw ---------- */
void UI_Update(float measured_c, float setpoint_c, uint8_t heater_pct, uint8_t iron_pct, uint8_t fan_pct, uint8_t overtemp) {
    (void)setpoint_c;
    uint32_t now = HAL_GetTick();

    /* header */
    LCD_FillRect(0, 0, LCD_W, HDR_H, C_BLUE);
    LCD_DrawString(4, 4, "HEATER CONTROLLER", C_WHITE, C_BLUE, 2);

    /* status badge on header right */
    if (overtemp) LCD_DrawString(244, 4, "ERR", C_RED, C_BLUE, 2);
    else if (Control_GetEnabled()) LCD_DrawString(238, 4, "RUN", C_GREEN, C_BLUE, 2);
    else LCD_DrawString(238, 4, "STOP", C_GRAY, C_BLUE, 2);

    /* temp / setpoint */
    uint16_t col_set = (cursor == 0) ? C_YELLOW : C_WHITE;
    sprintf(buf, "TEMP %6.1fC", measured_c);
    LCD_DrawString(4, 28, buf, C_WHITE, C_BLACK, 2);
    sprintf(buf, "SET  %6.1fC", setpoint);
    LCD_DrawString(4, 50, buf, col_set, C_BLACK, 2);

    /* bars */
    uint16_t col_iron = (cursor == 1) ? C_YELLOW : C_WHITE;
    uint16_t col_fan  = (cursor == 2) ? C_YELLOW : C_WHITE;
    sprintf(buf, "HEAT %3d%%", heater_pct);
    LCD_DrawString(4, 80, buf, C_GREEN, C_BLACK, 1);
    draw_bar(70, 80, 90, 10, heater_pct, C_GREEN);

    sprintf(buf, "IRON %3d%%", iron_pct);
    LCD_DrawString(4, 100, buf, col_iron, C_BLACK, 1);
    draw_bar(70, 100, 90, 10, iron_pct, C_ORANGE);

    sprintf(buf, "FAN  %3d%%", fan_pct);
    LCD_DrawString(4, 120, buf, col_fan, C_BLACK, 1);
    draw_bar(70, 120, 90, 10, fan_pct, C_CYAN);

    /* hints */
    LCD_DrawString(4, 160, "UP/DN: value", C_GRAY, C_BLACK, 1);
    LCD_DrawString(4, 172, "L/R: select", C_GRAY, C_BLACK, 1);

    /* trend graph, sample every 500 ms */
    if (hist_count < GRAPH_W) {
        if (now - last_sample >= 500) {
            last_sample = now;
            hist[hist_count++] = (int16_t)measured_c;
        }
    } else {
        if (now - last_sample >= 500) {
            last_sample = now;
            memmove(hist, hist + 1, sizeof(hist) - sizeof(hist[0]));
            hist[GRAPH_W - 1] = (int16_t)measured_c;
        }
    }
    draw_graph();
}