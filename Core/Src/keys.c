#include "keys.h"
#include "stm32g0xx_hal.h"

/* pins: SW1=PC3(UP), SW2=PC2(DOWN), SW3=PC1(LEFT), SW4=PC0(RIGHT) */
#define DEBOUNCE_MS 30

static uint8_t raw_up, raw_down, raw_left, raw_right;
static uint8_t deb_up, deb_down, deb_left, deb_right;
static uint8_t prev_up, prev_down, prev_left, prev_right;
static uint8_t edge_up, edge_down, edge_left, edge_right;
static uint32_t t_up, t_down, t_left, t_right;

static uint8_t read_pin(GPIO_TypeDef *port, uint16_t pin) {
    return (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET) ? 1 : 0; /* active low */
}

void Keys_Init(void) {
    raw_up = raw_down = raw_left = raw_right = 0;
    deb_up = deb_down = deb_left = deb_right = 0;
    prev_up = prev_down = prev_left = prev_right = 0;
    edge_up = edge_down = edge_left = edge_right = 0;
}

void Keys_Update(void) {
    uint32_t now = HAL_GetTick();
    uint8_t r;
    r = read_pin(SW1_GPIO_Port, SW1_Pin);
    if (r != raw_up) { raw_up = r; t_up = now; }
    if ((now - t_up) >= DEBOUNCE_MS) {
        if (raw_up && !deb_up) edge_up = 1;
        deb_up = raw_up;
    }
    r = read_pin(SW2_GPIO_Port, SW2_Pin);
    if (r != raw_down) { raw_down = r; t_down = now; }
    if ((now - t_down) >= DEBOUNCE_MS) {
        if (raw_down && !deb_down) edge_down = 1;
        deb_down = raw_down;
    }
    r = read_pin(SW3_GPIO_Port, SW3_Pin);
    if (r != raw_left) { raw_left = r; t_left = now; }
    if ((now - t_left) >= DEBOUNCE_MS) {
        if (raw_left && !deb_left) edge_left = 1;
        deb_left = raw_left;
    }
    r = read_pin(GPIOC, GPIO_PIN_0);
    if (r != raw_right) { raw_right = r; t_right = now; }
    if ((now - t_right) >= DEBOUNCE_MS) {
        if (raw_right && !deb_right) edge_right = 1;
        deb_right = raw_right;
    }
}

uint8_t Key_Up(void)   { return deb_up; }
uint8_t Key_Down(void) { return deb_down; }
uint8_t Key_Left(void)  { return deb_left; }
uint8_t Key_Right(void) { return deb_right; }

uint8_t Key_Up_Pressed(void) { uint8_t e = edge_up; edge_up = 0; return e; }
uint8_t Key_Down_Pressed(void) { uint8_t e = edge_down; edge_down = 0; return e; }
uint8_t Key_Left_Pressed(void) { uint8_t e = edge_left; edge_left = 0; return e; }
uint8_t Key_Right_Pressed(void) { uint8_t e = edge_right; edge_right = 0; return e; }