#ifndef FAN_H
#define FAN_H
#include <stdint.h>

/* two L293D channels: 0 = heater fan on XH500 (24 V), 1 = auxiliary fan on
   XH501. Both outputs sit on the 24 V rail, so the 12 V fan of XH501 is
   duty limited by FAN_CH1_MAX_PCT. */
#define FAN_CH0          0
#define FAN_CH1          1
#define FAN_CH1_MAX_PCT  50   /* 24 V * 50 % = 12 V at the connector */

void Fan_Init(void);
void Fan_SetSpeed(uint8_t ch, uint8_t percent);
uint8_t Fan_GetSpeed(uint8_t ch);
void Fan_SetAuto(uint8_t en);
uint8_t Fan_GetAuto(void);
/* auto mode: channel 0 follows the heater, channel 1 stays manual */
void Fan_Update(uint8_t heater_active);

#endif
