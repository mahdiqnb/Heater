#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdint.h>

/* "HEA3": the record layout changed when the second fan channel was added, so
   an old record is ignored and the defaults are loaded instead. */
#define SETTINGS_MAGIC 0x48454133UL

/* exactly 16 bytes = two flash doublewords */
typedef struct {
    uint32_t magic;
    int16_t  setpoint_x10; /* degC * 10, 0..5000 */
    uint8_t  iron_pct;     /* 0..100 */
    uint8_t  fan_pct;      /* 0..100, heater fan */
    uint8_t  fan2_pct;     /* 0..50,  aux fan (12 V fan on a 24 V channel) */
    uint8_t  enabled;      /* heater armed by the user */
    uint8_t  fan_auto;     /* 1 = follow the heater */
    uint8_t  pad;
    int16_t  kp_x10;       /* 0..200  -> 0.0 .. 20.0 */
    int16_t  ki_x100;      /* 0..500  -> 0.00 .. 5.00 */
    int16_t  kd_x10;       /* 0..200  -> 0.0 .. 20.0 */
} settings_t;

void Settings_Load(settings_t *s);
void Settings_Save(const settings_t *s);

#endif
