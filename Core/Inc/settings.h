#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdint.h>

/* "HEA4": the record grew to three flash doublewords (the old 16 byte record
 * silently dropped KD, because the struct was already 20 bytes), so an old
 * record is ignored and the defaults are loaded instead. */
#define SETTINGS_MAGIC 0x48454134UL

/* exactly 24 bytes = three flash doublewords */
typedef struct {
    uint32_t magic;
    int16_t  setpoint_x10; /* degC * 10, 0..5000 */
    uint8_t  iron_pct;     /* 0..100 */
    uint8_t  fan_pct;      /* 0..100, heater fan on XH500 */
    uint8_t  fan2_pct;     /* 0..50, box fan on XH501 (24 V channel) */
    uint8_t  enabled;      /* heater armed by the user */
    uint8_t  fan_auto;     /* 1 = heater fan follows the heater */
    uint8_t  fan2_mute;    /* box fan silenced by the user */
    int16_t  kp_x10;       /* 0..200  -> 0.0 .. 20.0 */
    int16_t  ki_x100;      /* 0..500  -> 0.00 .. 5.00 */
    int16_t  kd_x10;       /* 0..200  -> 0.0 .. 20.0 */
    uint8_t  rsv[6];       /* pad to 24 bytes */
} settings_t;

void Settings_Load(settings_t *s);
void Settings_Save(const settings_t *s);

#endif
