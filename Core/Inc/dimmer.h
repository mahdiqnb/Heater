#ifndef DIMMER_H
#define DIMMER_H
#include <stdint.h>

void Dimmer_Init(void);
void Dimmer_SetHeater(uint8_t percent); /* 0..100 */
void Dimmer_SetIron(uint8_t percent);   /* 0..100 */
uint8_t Dimmer_GetHeater(void);
uint8_t Dimmer_GetIron(void);
/* call from the 50us timer tick ISR */
void Dimmer_TickISR(void);
/* Call from the zero-cross EXTI callback. */
void Dimmer_ZeroCross(void);
/* 1 while mains zero crossings keep arriving (0 = no sync, nothing can fire). */
uint8_t Dimmer_ZcOk(void);

#endif
