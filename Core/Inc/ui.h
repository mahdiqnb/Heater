#ifndef UI_H
#define UI_H
#include <stdint.h>

void UI_Init(void);
void UI_Update(float measured_c, float setpoint_c, uint8_t heater_pct, uint8_t iron_pct, uint8_t fan_pct, uint8_t overtemp);
void UI_HandleKeys(void);

float UI_GetSetpoint(void);
uint8_t UI_GetIron(void);
uint8_t UI_GetFan(void);
uint8_t UI_GetEnabled(void);

#endif