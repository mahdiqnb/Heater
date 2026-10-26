#include "temp.h"
#include "main.h"
#include "stm32g0xx_hal.h"

#define SHT40_ADDR (0x44 << 1)

/* INA826 front end, Rg = 330R -> G = 1 + 49.4k/330 = 150.7 */
#define INA_GAIN  150.7f
#define ADC_VREF  3.3f
#define ADC_FULL  4095.0f

/* eight conversions per reading; the extremes are thrown away, so one noisy
   sample (the triac is switching 24V next door) cannot move the result */
#define TC_OS     8

/* EMF of the cold junction, type K, in mV: quadratic fit of the 0..80 C part
   (0.798 mV @20C, 1.611 @40C, 2.436 @60C) */
#define CJ_A      0.03970f
#define CJ_B      1.21e-5f

/* upper end of the inverse polynomial. The ADC saturates at 527 C
   (3.3V / 150.7 = 21.85 mV), so anything up to 22 mV is inside the useful
   range of the curve. */
#define E_MAX_MV  22.0f

extern I2C_HandleTypeDef hi2c1;
extern ADC_HandleTypeDef hadc1;

void Temp_Init(void) {
    /* offset/gain calibration is mandatory on STM32G0 before the first
       conversion, otherwise every reading is a few percent off */
    HAL_ADCEx_Calibration_Start(&hadc1);
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
    uint8_t cmd = 0xFD; /* high repeatability, ~8.3 ms conversion */
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT40_ADDR, &cmd, 1, 50) != HAL_OK) return 1;
    HAL_Delay(10);
    uint8_t buf[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT40_ADDR, buf, 6, 50) != HAL_OK) return 1;
    uint16_t traw = (uint16_t)((buf[0] << 8) | buf[1]);
    *amb = -45.0f + 175.0f * (float)traw / 65535.0f;
    return 0;
}

/* trimmed mean of TC_OS conversions */
static uint16_t tc_average(void) {
    uint16_t lo = 0xFFFF, hi = 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < TC_OS; i++) {
        uint16_t v = Temp_ReadADC();
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    sum -= lo;
    sum -= hi;
    return (uint16_t)(sum / (TC_OS - 2));
}

/* NIST ITS-90 inverse polynomial, type K, 0..500 C.
   Input in mV, output in degC (checked within 0.05 C against the table). */
static float k_inverse(float mv) {
    static const float d[10] = {
         0.0f,
         2.508355e1f,  7.860106e-2f, -2.503131e-1f,  8.315270e-2f,
        -1.228034e-2f,  9.804036e-4f, -4.413030e-5f,  1.057734e-6f,
        -1.052755e-8f
    };
    float t = d[9];
    for (int8_t i = 8; i >= 1; i--) t = t * mv + d[i];
    return t * mv;
}

float Temp_ReadHot(float ambient_c) {
    float v_tc = (float)tc_average() * ADC_VREF / (ADC_FULL * INA_GAIN);

    /* the thermocouple only reports hot minus cold junction, so the EMF of
       the cold junction itself has to be added before the inverse */
    float e = v_tc * 1000.0f;
    if (ambient_c > -40.0f && ambient_c < 100.0f) {
        e += CJ_A * ambient_c + CJ_B * ambient_c * ambient_c;
    }
    if (e < 0.0f) e = 0.0f;
    if (e > E_MAX_MV) e = E_MAX_MV;

    float hot = k_inverse(e);
    if (hot < 0.0f) hot = 0.0f;
    return hot;
}
