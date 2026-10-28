#include "temp.h"
#include "main.h"
#include "stm32g0xx_hal.h"

#define SHT40_ADDR (0x44 << 1)

/* INA826 front end, measured on the real board:
 *    25 C  -> 7.0 mV at PA0 (both junctions at 25 C, so the whole 7.0 mV is
 *             the amplifier output offset)
 *    450 C -> 1250 mV at PA0 (K-type EMF hot-cold = E(450)-E(25) =
 *             18.456 - 0.979 = 17.477 mV)
 *    => G = (1250 - 7) / 17.477 = 71.12, offset = 7.0 mV
 * The schematic's Rg = 330R implies G = 150.7, which read ~219 C at a real
 * 450 C: that is what made the PID chase an unreachable setpoint and the LCD
 * show a temperature that had nothing to do with the iron.
 *
 * HOW TO RE-CALIBRATE:
 * 1. Put the thermocouple in boiling water (100.0 C at sea level).
 * 2. Read dbg_adc_mv from the debugger (millivolts at PA0).
 * 3. K-type EMF at 100 C = 4.096 mV (NIST); cj_mv for the ambient is
 *    0.03970*amb + 1.21e-5*amb^2.
 * 4. INA_GAIN = (dbg_adc_mv - TC_OFFSET_MV) / (4.096 - cj_mv).
 */
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

/* Upper end of the inverse polynomial: it is the NITS 0..500 C fit, so it is
   only trusted up to E(500 C) = 20.64 mV. The ADC rails at a much higher
   equivalent temperature now ((3300-7)/71.12 = 46.3 mV), so this cap exists
   purely to keep the polynomial from being evaluated out of range. */
#define E_MAX_MV  20.64f

extern I2C_HandleTypeDef hi2c1;
extern ADC_HandleTypeDef hadc1;

/* --- SHT40 debug globals: watch these live in the debugger (Live Expressions) ---
   g_sht_temp_c : last temperature read from the SHT40, degC
   g_sht_humi_rh : last relative humidity read from the SHT40, %RH
   g_sht_ok     : 1 = the last transaction succeeded
   g_sht_err    : number of failed I2C transactions since power-on */
volatile float    g_sht_temp_c = 0.0f;
volatile float    g_sht_humi_rh = 0.0f;
volatile uint8_t  g_sht_ok = 0;
volatile uint16_t g_sht_err = 0;

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
    uint8_t cmd = 0xFD; /* single shot, high repeatability, ~8.3 ms conversion */
    if (HAL_I2C_Master_Transmit(&hi2c1, SHT40_ADDR, &cmd, 1, 50) != HAL_OK) {
        g_sht_ok = 0;
        g_sht_err++;
        return 1;
    }
    HAL_Delay(10);
    uint8_t buf[6];
    if (HAL_I2C_Master_Receive(&hi2c1, SHT40_ADDR, buf, 6, 50) != HAL_OK) {
        g_sht_ok = 0;
        g_sht_err++;
        return 1;
    }
    uint16_t traw = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t hraw = (uint16_t)((buf[3] << 8) | buf[4]);

    float t = -45.0f + 175.0f * (float)traw / 65535.0f;
    float rh = -6.0f + 125.0f * (float)hraw / 65535.0f;
    if (rh < 0.0f) rh = 0.0f;
    if (rh > 100.0f) rh = 100.0f;

    /* publish to the debug globals */
    g_sht_temp_c = t;
    g_sht_humi_rh = rh;
    g_sht_ok = 1;

    *amb = t;
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
   about 5.3 uV at the input, i.e. roughly 0.14 C, so 80 counts (~11 C) is
   a step no real thermocouple makes in one 100 ms cycle due to conduction.
   Real mains-burst spikes are still caught here, and anything that survives
   is clamped again by tc_smooth() before it reaches the display. */
#define TC_SPIKE_COUNTS 80

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

/* ------------------------------------------------------------------ filters */
/* Clamp-then-average. Every sample is first clamped to +/-OUTLIER_TOL_C
   around the current window mean and only then pushed into the moving
   average. A single huge glitch therefore moves the mean by at most
   OUTLIER_TOL_C/MA_LEN instead of dragging the whole display with it, while
   a real ramp still passes through almost unchanged (each sample is only
   clamped by the distance the element can physically travel). The output is
   additionally held inside DISP_HYST_C so a quiet reading does not flicker. */
#define MA_LEN        8       /* moving-average window (samples) */
#define OUTLIER_TOL_C 30.0f   /* max believable change vs. the window mean */
#define DISP_HYST_C   0.5f    /* output dead-band */

static float tc_smooth(float cand) {
    static float buf[MA_LEN];
    static float sum;
    static uint8_t idx;
    static uint8_t primed;
    static float disp;

    if (!primed) {
        for (uint8_t i = 0; i < MA_LEN; i++) buf[i] = cand;
        sum  = cand * (float)MA_LEN;
        disp = cand;
        primed = 1;
        return cand;
    }

    /* reject the impossible part of the sample before it enters the window */
    float mean = sum / (float)MA_LEN;
    if (cand > mean + OUTLIER_TOL_C) cand = mean + OUTLIER_TOL_C;
    if (cand < mean - OUTLIER_TOL_C) cand = mean - OUTLIER_TOL_C;

    sum -= buf[idx];
    buf[idx] = cand;
    sum += cand;
    if (++idx >= MA_LEN) idx = 0;

    float f = sum / (float)MA_LEN;
    if (f > disp + DISP_HYST_C || f < disp - DISP_HYST_C) disp = f;
    return disp;
}

float Temp_ReadHot(float ambient_c) {
    /* front end: raw counts -> PA0 millivolts -> subtract the measured 7 mV
       amplifier offset -> divide by the measured gain to recover the
       hot-minus-cold thermocouple EMF in mV. */
    float pa0_mv = (float)tc_deglitch() * ADC_VREF * 1000.0f / ADC_FULL;
    float e = (pa0_mv - TC_OFFSET_MV) / INA_GAIN;

    /* the thermocouple only reports hot minus cold junction, so the EMF of
       the cold junction itself has to be added before the inverse */
    if (ambient_c > -40.0f && ambient_c < 100.0f) {
        e += CJ_A * ambient_c + CJ_B * ambient_c * ambient_c;
    }
    if (e < 0.0f) e = 0.0f;
    if (e > E_MAX_MV) e = E_MAX_MV;

    float hot = k_inverse(e);
    if (hot < 0.0f) hot = 0.0f;

    /* clamp + moving average + output hysteresis live in tc_smooth() */
    return tc_smooth(hot);
}
