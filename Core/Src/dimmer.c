#include "dimmer.h"
#include "main.h"
#include "stm32g0xx_hal.h"

/* half-period in 50us ticks: 50Hz -> 10ms = 200 ticks.
 * The H11AA1 opto fires on RISING edge only (Falling callback is empty).
 * One zero-cross event arrives every 10ms = 200 ticks. */
#define HALF_PERIOD_TICKS     200

/* Minimum valid interval between two zero-cross events: 8ms = 160 ticks.
 * Anything closer is a spurious bounce/noise edge and is rejected. */
#define ZC_MIN_INTERVAL_TICKS 160UL

/* Safety: if no ZC for 25ms force gates off (2.5 mains periods). */
#define ZC_TIMEOUT_TICKS      500UL

/* ZC considered lost after 100ms of silence (generous window for slow LCD). */
#define ZC_LOST_TICKS         2000UL

/* CH1 (heater) full-on threshold.
 * Above this percentage the gate pin is held HIGH continuously instead of
 * using phase-angle pulses.  A short gate pulse at very high duty (delay < 5
 * ticks) can end before the TRIAC latches reliably, so the measured voltage
 * drops paradoxically.  Holding the gate HIGH permanently is safe: the TRIAC
 * still extinguishes at each natural zero-cross and re-fires immediately on
 * the next half-cycle, giving true full conduction. */
#define HEATER_FULLON_PCT     80u

/* Gate pulse width for phase-control range (0 .. HEATER_FULLON_PCT-1).
 * MOC3021 needs ≥10 µs; 3 ticks = 150 µs is reliable and short enough not
 * to overlap the next ZC event. */
#define GATE_PULSE_TICKS      3u

/* ----------------------------------------------------------------- state -- */
static volatile uint32_t dim_tick    = 0;
static volatile uint8_t  heater_pct  = 0;
static volatile uint8_t  iron_pct    = 0;

static volatile uint32_t fire1       = 0, fire2    = 0;
static volatile uint8_t  armed1      = 0, armed2   = 0;
static volatile uint32_t gate1_off   = 0, gate2_off = 0;
static volatile uint32_t last_zc_tick = 0;

/* 1 while CH1 gate is held HIGH in full-on mode */
static volatile uint8_t  ch1_fullon  = 0;

/* --------------------------------------------------------- RMS delay table -
 * Maps 0..100 % request to firing-angle delay in 50 µs ticks.
 * Inverse of the phase-control RMS equation:
 *   (Vout/Vin)^2 = (π - α + sin(2α)/2) / π
 *
 *   0 %  → 200 ticks  (full half-period, no conduction → 0 V)
 *   5 %  → 185 ticks  (~9.25 ms delay  → ~11 V RMS)
 *  50 %  → 126 ticks  (~6.3 ms delay   → ~110 V RMS)
 *  79 %  →  47 ticks  (last phase-ctrl entry before full-on)
 * ≥80 %  → full-on (gate held HIGH, see HEATER_FULLON_PCT)
 */
static const uint8_t rms_delay_ticks[101] = {
    200, 195, 192, 190, 187, 185, 184, 182, 180, 178,
    177, 175, 174, 172, 171, 169, 168, 167, 165, 164,
    163, 161, 160, 159, 158, 156, 155, 154, 153, 151,
    150, 149, 148, 147, 145, 144, 143, 142, 141, 139,
    138, 137, 136, 135, 134, 132, 131, 130, 129, 128,
    126, 125, 124, 123, 122, 120, 119, 118, 117, 115,
    114, 113, 112, 110, 109, 108, 106, 105, 104, 102,
    101, 100,  98,  97,  95,  94,  92,  91,  89,  87,
     86,  84,  82,  81,  79,  77,  75,  73,  71,  68,
     66,  63,  61,  58,  55,  51,  47,  43,  37,  29,
      0
};

/* Returns firing delay in ticks (used only for phase-control range). */
static uint32_t RmsDelayTicks(uint8_t percent) {
    if (percent >= 100) return 0;
    return (uint32_t)rms_delay_ticks[percent];
}

/* ================================================================== API === */

void Dimmer_Init(void) {
    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
    dim_tick    = 0;
    last_zc_tick = 0;
    heater_pct  = 0;
    iron_pct    = 0;
    fire1 = fire2 = 0;
    armed1 = armed2 = 0;
    gate1_off = gate2_off = 0;
    ch1_fullon  = 0;
}

void Dimmer_SetHeater(uint8_t percent) {
    if (percent > 100) percent = 100;
    heater_pct = percent;

    /* Enter or leave full-on mode immediately so the gate state is correct
     * even between zero-cross events. */
    if (percent >= HEATER_FULLON_PCT) {
        if (!ch1_fullon) {
            ch1_fullon = 1;
            /* Cancel any pending phase-control pulse and hold gate HIGH. */
            armed1    = 0;
            gate1_off = 0;
            HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        }
    } else {
        if (ch1_fullon) {
            ch1_fullon = 0;
            /* Gate will be handled by the next ZeroCross event. */
            HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
        }
    }
}

void Dimmer_SetIron(uint8_t percent) {
    if (percent > 100) percent = 100;
    iron_pct = percent;
}

uint8_t Dimmer_GetHeater(void) { return heater_pct; }
uint8_t Dimmer_GetIron(void)   { return iron_pct;   }

uint8_t Dimmer_ZcOk(void) {
    return ((dim_tick - last_zc_tick) < ZC_LOST_TICKS) ? 1 : 0;
}

/* ============================================================= ISR handlers */

void Dimmer_TickISR(void) {
    dim_tick++;

    /* Safety: no ZC for 25 ms → force gates off (except full-on CH1). */
    if ((dim_tick - last_zc_tick) > ZC_TIMEOUT_TICKS) {
        if (!ch1_fullon) {
            HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
            armed1    = 0;
            gate1_off = 0;
        }
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
        armed2    = 0;
        gate2_off = 0;
    }

    /* CH1 full-on: nothing to do — gate is already HIGH and stays there. */

    /* CH1 phase-control: fire when delay expires */
    if (armed1 && dim_tick >= fire1) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        gate1_off = dim_tick + GATE_PULSE_TICKS;
        armed1 = 0;
    }
    /* CH2 phase-control: fire when delay expires */
    if (armed2 && dim_tick >= fire2) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
        gate2_off = dim_tick + GATE_PULSE_TICKS;
        armed2 = 0;
    }

    /* End gate pulses (phase-control only, not full-on) */
    if (!ch1_fullon && gate1_off != 0 && dim_tick >= gate1_off) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
        gate1_off = 0;
    }
    if (gate2_off != 0 && dim_tick >= gate2_off) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
        gate2_off = 0;
    }
}

void Dimmer_ZeroCross(void) {
    /* Reject spurious edge closer than 8 ms to the previous one. */
    if ((dim_tick - last_zc_tick) < ZC_MIN_INTERVAL_TICKS) return;

    /* ---- CH2 (iron): always phase-control ---- */
    HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
    armed2    = 0;
    gate2_off = 0;

    if (iron_pct == 0) {
        /* nothing to arm */
    } else {
        uint32_t delay = RmsDelayTicks(iron_pct);
        if (delay == 0) {
            HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
            gate2_off = dim_tick + GATE_PULSE_TICKS;
        } else {
            fire2  = dim_tick + delay;
            armed2 = 1;
        }
    }

    /* ---- CH1 (heater) ---- */
    if (ch1_fullon) {
        /* Full-on mode: gate was already HIGH; it was momentarily reset by
         * the MOC3021 TRIAC-off at ZC, so drive it HIGH again immediately. */
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
    } else {
        /* Phase-control mode */
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
        armed1    = 0;
        gate1_off = 0;

        if (heater_pct == 0) {
            /* nothing to arm */
        } else {
            uint32_t delay = RmsDelayTicks(heater_pct);
            if (delay == 0) {
                HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
                gate1_off = dim_tick + GATE_PULSE_TICKS;
            } else {
                fire1  = dim_tick + delay;
                armed1 = 1;
            }
        }
    }

    last_zc_tick = dim_tick;
}
