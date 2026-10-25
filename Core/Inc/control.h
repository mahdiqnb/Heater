#ifndef CONTROL_H
#define CONTROL_H
#include <stdint.h>

void Control_Init(void);
void Control_Update(float measured_c, float setpoint_c);
uint8_t Control_GetHeaterPercent(void);
void Control_SetEnabled(uint8_t en);
uint8_t Control_GetEnabled(void);

#endif