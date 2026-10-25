#include "temp.h"
#include "main.h"
#include "stm32g0xx_hal.h"
#include <math.h>

#define SHT40_ADDR (0x44 << 1)
#define INA_GAIN 151.0f
#define TC_UV_PER_C 41.0f /* K-type approx */

extern I2C_HandleTypeDef hi2c1;
extern ADC_HandleTypeDef hadc1;

void Temp_Init(void) {
    /* ADC already init by MX_ADC1_Init */
}

uint16_t Temp_ReadADC(void) {
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
        uint16_t v = HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
        return v;
    }
    HAL_ADC_Stop(&hadc1);
    return 0;
}

uint8_t Temp_ReadAmbient(float *amb) {
    uint8_t cmd = 0xFD; /* high repeatability */
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT40_ADDR, &cmd, 1, 50) != HAL_OK) return 1;
    HAL_Delay(10);
    uint8_t buf[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT40_ADDR, buf, 6, 50) != HAL_OK) return 1;
    uint16_t traw = (buf[0] << 8) | buf[1];
    float t = -45.0f + 175.0f * (float)traw / 65535.0f;
    *amb = t;
    return 0;
}

float Temp_ReadHot(float ambient_c) {
    uint16_t raw = Temp_ReadADC();
    float vadc = (float)raw * 3.3f / 4095.0f;
    float vtc = vadc / INA_GAIN; /* volts */
    float delta_c = vtc / (TC_UV_PER_C * 1e-6f); /* deg C above cold junction */
    float hot = ambient_c + delta_c;
    if (hot < 0) hot = 0;
    if (hot > 600) hot = 600;
    return hot;
}