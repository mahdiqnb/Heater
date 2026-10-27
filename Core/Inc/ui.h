#ifndef UI_H
#define UI_H
#include <stdint.h>

void UI_Splash(void);   /* boot logo; blocks for a couple of seconds */
void UI_Init(void);

/* call once per main-loop pass: measured temperature, heater/iron/fan duty
   and the fault flag (0 = none, 1 = over temperature, 2 = sensor/runaway) */
void UI_Update(float measured_c, uint8_t heater_pct, uint8_t iron_pct,
               uint8_t fan_pct, uint8_t fan2_pct, uint8_t fault);
void UI_HandleKeys(void);

float   UI_GetSetpoint(void);
uint8_t UI_GetIron(void);
uint8_t UI_GetFan(void);
uint8_t UI_GetFan2(void);
/* user intent for the heater (the SETUP page switch), not the PID state:
   main.c uses it to restore the heater after an over-temperature trip */
uint8_t UI_GetEnabled(void);
/* manual heater override: 1 = user is controlling dimmer directly */
uint8_t UI_GetHeaterManual(void);
uint8_t UI_GetHeaterManualPct(void);

#endif
