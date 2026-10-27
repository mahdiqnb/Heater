#include "temp.h"
#include "main.h"
#include "stm32g0xx_hal.h"

#define SHT40_ADDR (0x44 << 1)

/* INA826 front end, Rg = 330R -> G = 1 + 49.4k/330 = 150.7
 *
 * HOW TO CALIBRATE:
 * 1. Put the thermocouple in boiling water (100.0 C at sea level).
 * 2. Read dbg_adc_mv from the debugger (millivolts at PA0).
 * 3. At 100 C, K-type EMF = 4.096 mV (from NIST table).
 * 4. INA_GAIN = dbg_adc_mv / 4.096
 *    e.g. if dbg_adc_mv = 650 → INA_GAIN = 650/4.096 = 158.7
 *
 * Alternatively use the identity:
 *    INA_GAIN = 1 + 49400 / Rg_actual
 * Measure Rg with a meter and substitute.
 */
#define INA_GAIN  150.7f
#define ADC_VREF  3.3f
#define ADC_FULL  4095.0f

/* One mains cycle at 50 Hz is 20 ms. The triacs switch 24 V next to the
   thermocouple cable, so a conversion taken at one firing phase sees a
   different amount of coupling than one taken at another phase. Sampling at
   ~1 ms intervals across a whole cycle visits every phase, so the average is
   the same in every reading instead of drifting with the dimmer duty -- that
   drift is what made the heater and the iron appear to interact.

   The ~19 ms this blocks happens inside the 100 ms control tick, so the loop
   period stays at ~100 ms and the key scan at the top of the loop is not
   affected. */
#define TC_OS        20
#define TC_SAMPLE_MS 1

/* the extremes are thrown away, so one noisy sample cannot move the result */

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
    /* Acquisition window. The .ioc leaves the common sampling time at the
       default 1.5 cycles, which is far too short for this front end: the
       sample-and-hold then captures whatever the triac just coupled into the
       cable and the reading jumps by a hundred degrees. 39.5 cycles gives a
       real integration window. Set here (not only in the .ioc) so a CubeMX
       regenerate cannot undo it. */
    ADC_ChannelConfTypeDef ch = { 0 };
    hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_39CYCLES_5;
    hadc1.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_39CYCLES_5;
    if (HAL_ADC_Init(&hadc1) == HAL_OK) {
        ch.Channel = ADC_CHANNEL_0;
        ch.Rank = ADC_REGULAR_RANK_1;
        ch.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
        HAL_ADC_ConfigChannel(&hadc1, &ch);
    }

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

/* trimmed mean of TC_OS conversions spread over one mains cycle */
static uint16_t tc_average(void) {
    uint16_t lo = 0xFFFF, hi = 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < TC_OS; i++) {
        if (i)
            HAL_Delay(TC_SAMPLE_MS);
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

/* A single conversion must not be allowed to jump the display. One count is
   about 5.3 uV at the input, i.e. roughly 0.14 C, so 200 counts (~28 C) is
   a step no real thermocouple makes in one 100 ms cycle due to conduction.
   Raised from the original 60 so fast-heating elements are not artificially
   slowed down. Real mains-burst spikes are still caught by tc_deglitch(). */
#define TC_SPIKE_COUNTS 200

/* Maximum temperature change the output is allowed to follow per 100 ms
   control cycle. The element can heat at >30 C/s when at full power, so
   50 C/cycle (= 500 C/s) is high enough to track real heating while still
   rejecting single-cycle glitches from a loose thermocouple screw.
   The original 10 C/cycle (100 C/s) was too conservative: it caused the
   displayed temperature to lag ~10 s behind reality at full power, making
   the PID overshoot badly because it thought the element was still cold. */
#define TC_MAX_STEP_C   50.0f

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    uint16_t t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    return b;
}

/* newest conversion, unless it is an outlier compared to the previous two:
   then the median is used. Normal readings pass through untouched, so this
   adds no lag -- it only removes the one-off spikes. */
static uint16_t tc_deglitch(void) {
    static uint16_t hist[3];
    static uint8_t n;
    uint16_t raw = tc_average();

    hist[2] = hist[1];
    hist[1] = hist[0];
    hist[0] = raw;
    if (n < 3) n++;
    if (n < 3) return raw;

    uint16_t med = median3(hist[0], hist[1], hist[2]);
    int32_t d = (int32_t)raw - (int32_t)med;
    if (d > TC_SPIKE_COUNTS || d < -TC_SPIKE_COUNTS) return med;
    return raw;
}

float Temp_ReadHot(float ambient_c) {
    static float last = -1000.0f;
    float v_tc = (float)tc_deglitch() * ADC_VREF / (ADC_FULL * INA_GAIN);

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

    /* last line of defence: a jump this large in one cycle is not the element
       warming up, it is a glitch (loose thermocouple screw, mains burst) */
    if (last > -100.0f) {
        if (hot > last + TC_MAX_STEP_C) hot = last + TC_MAX_STEP_C;
        else if (hot < last - TC_MAX_STEP_C) hot = last - TC_MAX_STEP_C;
    }

    last = hot;
    return hot;
}
