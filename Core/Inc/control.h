#ifndef CONTROL_H
#define CONTROL_H
#include <stdint.h>

/* Control_GetFault() values */
#define CTRL_FAULT_NONE   0
#define CTRL_FAULT_SENSOR 2  /* power in, no heat out: open sensor or element */

void Control_Init(void);
/* call every 100 ms; dt is measured internally from the tick.
   ambient_c is the SHT40 reading and is only used by the sensor check */
void Control_Update(float measured_c, float setpoint_c, float ambient_c);
uint8_t Control_GetHeaterPercent(void);
void Control_SetEnabled(uint8_t en);
uint8_t Control_GetEnabled(void);
void Control_SetGains(float p, float i, float d);
void Control_GetGains(float *p, float *i, float *d);
uint8_t Control_GetFault(void);
void Control_ClearFault(void);

#endif
