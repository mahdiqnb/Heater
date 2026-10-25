#include "control.h"
#include "dimmer.h"
#include "stm32g0xx_hal.h"

#define KP 2.0f
#define KI 0.05f
#define KD 1.0f
#define I_MAX 50.0f

static float integral = 0;
static float prev_err = 0;
static uint32_t prev_tick = 0;
static uint8_t enabled = 0;
static uint8_t heater_pct = 0;

void Control_Init(void) {
    integral = 0; prev_err = 0; prev_tick = HAL_GetTick();
    enabled = 0; heater_pct = 0;
}

void Control_SetEnabled(uint8_t en) { enabled = en; if (!en) { integral = 0; heater_pct = 0; Dimmer_SetHeater(0); } }
uint8_t Control_GetEnabled(void) { return enabled; }

void Control_Update(float measured_c, float setpoint_c) {
    if (!enabled || setpoint_c <= 0) {
        heater_pct = 0;
        Dimmer_SetHeater(0);
        return;
    }
    /* overtemp protection */
    if (measured_c > setpoint_c + 40.0f || measured_c > 520.0f) {
        heater_pct = 0;
        Dimmer_SetHeater(0);
        integral = 0;
        return;
    }
    float err = setpoint_c - measured_c;
    uint32_t now = HAL_GetTick();
    float dt = (float)(now - prev_tick) / 1000.0f;
    if (dt <= 0) dt = 0.001f;
    if (dt > 1.0f) { integral = 0; dt = 1.0f; }
    integral += err * dt;
    if (integral > I_MAX) integral = I_MAX;
    if (integral < -I_MAX) integral = -I_MAX;
    float deriv = (err - prev_err) / dt;
    float out = KP * err + KI * integral + KD * deriv;
    if (out < 0) out = 0;
    if (out > 100) out = 100;
    heater_pct = (uint8_t)out;
    Dimmer_SetHeater(heater_pct);
    prev_err = err;
    prev_tick = now;
}

uint8_t Control_GetHeaterPercent(void) { return heater_pct; }