#include "dimmer.h"
#include "main.h"
#include "stm32g0xx_hal.h"

/* half-period in 50us ticks: 50Hz -> 10ms = 200 ticks */
#define HALF_PERIOD_TICKS 200

static volatile uint32_t dim_tick = 0;
static volatile uint8_t heater_pct = 0;
static volatile uint8_t iron_pct = 0;
static volatile uint32_t fire1 = 0, fire2 = 0;
static volatile uint8_t armed1 = 0, armed2 = 0;
static volatile uint8_t last_zc_state = 0;
static volatile uint32_t last_zc_tick = 0;

void Dimmer_Init(void) {
    /* ensure outputs low */
    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET); /* PB15 */
    HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET); /* PC6 */
}

void Dimmer_SetHeater(uint8_t percent) {
    if (percent > 100) percent = 100;
    heater_pct = percent;
}
void Dimmer_SetIron(uint8_t percent) {
    if (percent > 100) percent = 100;
    iron_pct = percent;
}
uint8_t Dimmer_GetHeater(void) { return heater_pct; }
uint8_t Dimmer_GetIron(void) { return iron_pct; }

void Dimmer_ZeroCross(void) {
    /* called on zero-cross event */
    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
    /* schedule firing */
    if (heater_pct == 0) {
        armed1 = 0;
    } else if (heater_pct >= 100) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        armed1 = 0;
    } else {
        fire1 = dim_tick + (HALF_PERIOD_TICKS - (uint32_t)heater_pct * HALF_PERIOD_TICKS / 100);
        armed1 = 1;
    }
    if (iron_pct == 0) {
        armed2 = 0;
    } else if (iron_pct >= 100) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
        armed2 = 0;
    } else {
        fire2 = dim_tick + (HALF_PERIOD_TICKS - (uint32_t)iron_pct * HALF_PERIOD_TICKS / 100);
        armed2 = 1;
    }
    last_zc_tick = dim_tick;
}

void Dimmer_TickISR(void) {
    dim_tick++;
    /* zero-cross detection by sampling PC7 with debounce */
    uint8_t s = (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_7) == GPIO_PIN_SET) ? 1 : 0;
    if (s != last_zc_state) {
        last_zc_state = s;
        if (dim_tick - last_zc_tick >= 100) { /* 5ms debounce */
            Dimmer_ZeroCross();
        }
    }
    /* firing */
    if (armed1 && dim_tick >= fire1) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        armed1 = 0;
    }
    if (armed2 && dim_tick >= fire2) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
        armed2 = 0;
    }
}