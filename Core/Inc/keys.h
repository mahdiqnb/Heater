#ifndef KEYS_H
#define KEYS_H
#include "main.h"
#include <stdint.h>

/* Active-low (pulled up). Returns 1 while the key is held. */
uint8_t Key_Up(void);
uint8_t Key_Down(void);
uint8_t Key_Left(void);
uint8_t Key_Right(void);

/* One step per call: 1 on the press, and again on every repeat tick while the
 * key is held down (see the auto-repeat constants in keys.c). */
uint8_t Key_Up_Pressed(void);
uint8_t Key_Down_Pressed(void);
uint8_t Key_Left_Pressed(void);
uint8_t Key_Right_Pressed(void);

void Keys_Init(void);
void Keys_Update(void); /* call in main loop */

#endif
