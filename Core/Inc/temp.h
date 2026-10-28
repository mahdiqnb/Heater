#ifndef TEMP_H
#define TEMP_H
#include <stdint.h>

/* INA826 front end, calibrated on the real board (see temp.c for the
   derivation and the re-calibration procedure). Kept here so the debug
   monitor in main.c uses exactly the same numbers as the control loop. */
#define TC_OFFSET_MV  7.0f   /* PA0 output offset of the front end */
#define INA_GAIN      71.12f /* measured; the schematic implies 150.7 */

/* SHT40 debug globals (updated by Temp_ReadAmbient) */
extern volatile float    g_sht_temp_c;  /* degC */
extern volatile float    g_sht_humi_rh; /* %RH */
extern volatile uint8_t  g_sht_ok;      /* 1 = last I2C read OK */
extern volatile uint16_t g_sht_err;     /* failed reads counter */

void Temp_Init(void);
/* read SHT40 ambient temp (deg C) into *amb; returns 0 on success */
uint8_t Temp_ReadAmbient(float *amb);
/* read thermocouple hot temp (deg C) using ambient as cold junction */
float Temp_ReadHot(float ambient_c);
/* raw ADC (0..4095) */
uint16_t Temp_ReadADC(void);

#endif