#ifndef KEYS_H
#define KEYS_H
#include "main.h"
#include <stdint.h>

/* Active-low (pulled up). Returns 1 if pressed. */
uint8_t Key_Up(void);
uint8_t Key_Down(void);
uint8_t Key_Left(void);
uint8_t Key_Right(void);

/* Debounced single-shot: returns 1 once per press */
uint8_t Key_Up_Pressed(void);
uint8_t Key_Down_Pressed(void);
uint8_t Key_Left_Pressed(void);
uint8_t Key_Right_Pressed(void);

void Keys_Init(void);
void Keys_Update(void); /* call in main loop */

#endif