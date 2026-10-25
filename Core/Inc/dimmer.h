#ifndef DIMMER_H
#define DIMMER_H
#include <stdint.h>

void Dimmer_Init(void);
void Dimmer_SetHeater(uint8_t percent); /* 0..100 */
void Dimmer_SetIron(uint8_t percent);   /* 0..100 */
uint8_t Dimmer_GetHeater(void);
uint8_t Dimmer_GetIron(void);
/* call from 100us timer ISR */
void Dimmer_TickISR(void);
/* zero-cross handling (call from EXTI) */
void Dimmer_ZeroCross(void);

#endif