#include "fan.h"
#include "main.h"
#include "stm32g0xx_hal.h"

extern TIM_HandleTypeDef htim1;  /* PC8 ENABLE1 */
extern TIM_HandleTypeDef htim17; /* PB9 ENABLE2 */

static uint8_t speed = 0;
static uint8_t auto_mode = 1;

void Fan_Init(void) {
    /* directions: Fan1 INPUT1=PB6 high, INPUT2=PC11 low; Fan2 INPUT3=PC10 high, INPUT4=PB3 low */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_11, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim17, TIM_CHANNEL_1);
    Fan_SetSpeed(0);
}

void Fan_SetSpeed(uint8_t percent) {
    if (percent > 100) percent = 100;
    speed = percent;
    uint32_t duty = (uint32_t)percent * 65535 / 100;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, duty);
    __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, duty);
}
uint8_t Fan_GetSpeed(void) { return speed; }
void Fan_SetAuto(uint8_t en) { auto_mode = en; }
uint8_t Fan_GetAuto(void) { return auto_mode; }

void Fan_Update(uint8_t heater_active) {
    if (auto_mode) {
        if (heater_active) {
            if (speed < 40) Fan_SetSpeed(40);
        } else {
            if (speed > 0) Fan_SetSpeed(0);
        }
    }
}