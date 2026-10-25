#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdint.h>

typedef struct {
    uint32_t magic;
    int16_t  setpoint_x10; /* degC * 10 */
    uint8_t  iron_pct;
    uint8_t  fan_pct;
    uint8_t  enabled;
    uint8_t  reserved[3];
} settings_t;

void Settings_Load(settings_t *s);
void Settings_Save(const settings_t *s);

#endif