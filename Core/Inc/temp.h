#ifndef TEMP_H
#define TEMP_H
#include <stdint.h>

void Temp_Init(void);
/* read SHT40 ambient temp (deg C) into *amb; returns 0 on success */
uint8_t Temp_ReadAmbient(float *amb);
/* read thermocouple hot temp (deg C) using ambient as cold junction */
float Temp_ReadHot(float ambient_c);
/* raw ADC (0..4095) */
uint16_t Temp_ReadADC(void);

#endif