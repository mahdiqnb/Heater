#ifndef FAN_H
#define FAN_H
#include <stdint.h>

void Fan_Init(void);
void Fan_SetSpeed(uint8_t percent); /* 0..100 */
uint8_t Fan_GetSpeed(void);
void Fan_SetAuto(uint8_t en);
uint8_t Fan_GetAuto(void);
void Fan_Update(uint8_t heater_active);

#endif