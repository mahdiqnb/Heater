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

/* The physical press only -- auto-repeat ticks never set these. Read them
   every pass so no press stays pending; switches and page changes act only
   on these. */
uint8_t Key_Left_New(void);
uint8_t Key_Right_New(void);

/* 1 while the key has been held >= 3 s: values take a bigger step then. */
uint8_t Key_Left_Fast(void);
uint8_t Key_Right_Fast(void);

/* How long the key has been held in ms (0 when released): the menu uses it
   to grow the step size the longer the key stays down. */
uint32_t Key_Left_HoldMs(void);
uint32_t Key_Right_HoldMs(void);

void Keys_Init(void);
void Keys_Update(void); /* call in main loop */

#endif
