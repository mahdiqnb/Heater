#include "control.h"
#include "dimmer.h"
#include "stm32g0xx_hal.h"

/* P  = kp * error            [% output per degC]
 * I  = ki * integral(error)  [% output, accumulated and clamped to 0..100]
 * D  = -kp * kd * dT/dt      [%] (derivative on the measurement, textbook
 *                                form: kd is a derivative time in seconds)   */
#define KP_DEF 2.0f
#define KI_DEF 0.05f
#define KD_DEF 1.0f

/* the element is switched by a triac, so a jump from 0 to 100 % is legal but
   makes the loop hit the setpoint hard and overshoot */
#define OUT_SLEW_PCT_S 60.0f

/* low pass on the derivative: the raw dT/dt of a 12-bit reading is useless */
#define DERIV_TAU_S    1.5f

/* Protection: this much demand for this long must put the measured
   temperature clearly above the SHT40 ambient. A disconnected or shorted
   thermocouple reads "room temperature" for ever, and a broken element never
   heats at all; in both cases power goes in and no heat comes out.
   Comparing against the ambient (instead of "is it still rising") is what
   keeps a heater that simply cannot reach its setpoint from tripping this. */
#define RUNAWAY_MS       30000UL
#define RUNAWAY_DUTY     60
#define RUNAWAY_ERR_C    15.0f
#define RUNAWAY_DELTA_C  5.0f
#define ABS_MAX_C       520.0f

static float kp = KP_DEF, ki = KI_DEF, kd = KD_DEF;
static float integ;        /* % output, 0..100 */
static float d_term;       /* %, filtered */
static float prev_meas;
static float out_prev;
static uint8_t primed;
static uint32_t prev_ms;

static uint8_t enabled;
static uint8_t fault;
static uint8_t heater_pct;

static uint8_t  rise_active;
static uint32_t rise_ms;

void Control_Init(void) {
    kp = KP_DEF; ki = KI_DEF; kd = KD_DEF;
    integ = 0.0f; d_term = 0.0f; prev_meas = 0.0f; out_prev = 0.0f;
    primed = 0; prev_ms = HAL_GetTick();
    enabled = 0; fault = CTRL_FAULT_NONE; heater_pct = 0;
    rise_active = 0; rise_ms = 0;
}

void Control_SetGains(float p, float i, float d) {
    if (p < 0.0f) p = 0.0f;
    if (i < 0.0f) i = 0.0f;
    if (d < 0.0f) d = 0.0f;
    if (p != kp || i != ki || d != kd) {
        kp = p; ki = i; kd = d;
        /* a gain change makes the accumulated history meaningless, but the
           heater keeps the output it had: no bump on the element */
        integ = out_prev;
        d_term = 0.0f;
    }
}

void Control_GetGains(float *p, float *i, float *d) {
    if (p) *p = kp;
    if (i) *i = ki;
    if (d) *d = kd;
}

void Control_SetEnabled(uint8_t en) {
    en = en ? 1 : 0;
    if (en != enabled) {
        enabled = en;
        primed = 0;
        if (!en) {
            integ = 0.0f; d_term = 0.0f; out_prev = 0.0f;
            heater_pct = 0;
            rise_active = 0;
            Dimmer_SetHeater(0);
        }
    }
}
uint8_t Control_GetEnabled(void) { return enabled; }
uint8_t Control_GetFault(void) { return fault; }
void Control_ClearFault(void) {
    fault = CTRL_FAULT_NONE;
    rise_active = 0;
}

static void shut_down(void) {
    heater_pct = 0;
    integ = 0.0f;
    d_term = 0.0f;
    out_prev = 0.0f;
    Dimmer_SetHeater(0);
}

void Control_Update(float measured_c, float setpoint_c, float ambient_c) {
    uint32_t now = HAL_GetTick();
    float dt = (float)(now - prev_ms) / 1000.0f;
    prev_ms = now;
    if (dt < 0.02f) dt = 0.02f;
    if (dt > 0.50f) dt = 0.50f;

    /* absolute limit: never trust the setpoint while the sensor reads hot */
    if (measured_c > ABS_MAX_C) {
        fault = CTRL_FAULT_NONE; /* the overtemp logic in main owns this one */
        shut_down();
        prev_meas = measured_c;
        primed = 1;
        return;
    }

    if (!enabled || fault != CTRL_FAULT_NONE || setpoint_c <= 0.0f) {
        shut_down();
        prev_meas = measured_c;
        primed = 1;
        return;
    }

    float err = setpoint_c - measured_c;

    /* ---- D: derivative of the measurement, not of the error, so a setpoint
       change does not kick the triac ---- */
    if (primed) {
        float dm = (measured_c - prev_meas) / dt;
        float target = -kp * kd * dm;
        float a = dt / (DERIV_TAU_S + dt);
        d_term += a * (target - d_term);
    } else {
        d_term = 0.0f;
        primed = 1;
    }

    /* ---- I: conditional integration, the classic anti-windup ---- */
    float di = ki * err * dt;
    integ += di;
    if (integ > 100.0f) integ = 100.0f;
    if (integ < 0.0f) integ = 0.0f;

    float out = kp * err + integ + d_term;

    if (out > 100.0f) {
        out = 100.0f;
        if (err > 0.0f) integ -= di;   /* do not wind up against the ceiling */
    } else if (out < 0.0f) {
        out = 0.0f;
        if (err < 0.0f) integ -= di;
    }
    if (integ > 100.0f) integ = 100.0f;
    if (integ < 0.0f) integ = 0.0f;

    /* ---- rate limit, and do not let the integrator run ahead of it ---- */
    float step = OUT_SLEW_PCT_S * dt;
    if (out > out_prev + step) {
        out = out_prev + step;
        if (err > 0.0f) integ -= di;
    } else if (out < out_prev - step) {
        out = out_prev - step;
        if (err < 0.0f) integ -= di;
    }
    if (integ > 100.0f) integ = 100.0f;
    if (integ < 0.0f) integ = 0.0f;

    heater_pct = (uint8_t)(out + 0.5f);
    out_prev = out;
    prev_meas = measured_c;
    Dimmer_SetHeater(heater_pct);

    /* ---- runaway / open sensor detector ---- */
    if (heater_pct >= RUNAWAY_DUTY && err > RUNAWAY_ERR_C) {
        if (!rise_active) {
            rise_active = 1;
            rise_ms = now;
        } else if ((now - rise_ms) >= RUNAWAY_MS) {
            float ambc = (ambient_c > -40.0f && ambient_c < 100.0f) ? ambient_c : 25.0f;
            if ((measured_c - ambc) < RUNAWAY_DELTA_C) {
                fault = CTRL_FAULT_SENSOR;
                shut_down();
            } else {
                rise_ms = now;   /* it is heating: check again in 30 s */
            }
        }
    } else {
        rise_active = 0;
    }
}

uint8_t Control_GetHeaterPercent(void) { return heater_pct; }
