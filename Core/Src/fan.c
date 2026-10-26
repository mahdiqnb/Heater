#include "fan.h"
#include "main.h"
#include "stm32g0xx_hal.h"

extern TIM_HandleTypeDef htim1;  /* PC8 ENABLE1 -> XH500, heater fan */
extern TIM_HandleTypeDef htim17; /* PB9 ENABLE2 -> XH501, aux fan    */

/* Both L293D outputs are fed from VS = +24 V, so the 12 V fan on XH501 must
   never see more than half duty. This is a hard clamp in the driver layer:
   no UI or auto mode can go around it. */
#define FAN_AUX_MAX 50

static uint8_t speed[2] = {0, 0};
static uint8_t auto_mode = 1;

void Fan_Init(void) {
    /* direction pins are plain GPIO: they must be set up as outputs before
       driving them, otherwise the L293D inputs float at their reset state */
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = GPIO_PIN_6 | GPIO_PIN_3; /* PB6 = INPUT1, PB3 = INPUT4 */
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_11 | GPIO_PIN_10; /* PC11 = INPUT2, PC10 = INPUT3 */
    HAL_GPIO_Init(GPIOC, &g);

    /* directions: Fan1 INPUT1=PB6 high, INPUT2=PC11 low; Fan2 INPUT3=PC10 high, INPUT4=PB3 low */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_11, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim17, TIM_CHANNEL_1);
    Fan_SetSpeed(FAN_CH0, 0);
    Fan_SetSpeed(FAN_CH1, 0);
}

void Fan_SetSpeed(uint8_t ch, uint8_t percent) {
    if (ch > FAN_CH1) return;
    uint8_t max = (ch == FAN_CH1) ? FAN_AUX_MAX : 100;
    if (percent > max) percent = max;
    speed[ch] = percent;
    uint32_t duty = (uint32_t)percent * 65535UL / 100UL;
    if (ch == FAN_CH0) {
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, duty);
    } else {
        __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, duty);
    }
}

uint8_t Fan_GetSpeed(uint8_t ch) { return (ch > FAN_CH1) ? 0 : speed[ch]; }
void Fan_SetAuto(uint8_t en) { auto_mode = en ? 1 : 0; }
uint8_t Fan_GetAuto(void) { return auto_mode; }

void Fan_Update(uint8_t heater_active) {
    if (!auto_mode) return;
    /* only the heater fan follows the heater; the aux fan keeps its value */
    if (heater_active) {
        if (speed[FAN_CH0] < 40) Fan_SetSpeed(FAN_CH0, 40);
    } else {
        if (speed[FAN_CH0] > 0) Fan_SetSpeed(FAN_CH0, 0);
    }
}
