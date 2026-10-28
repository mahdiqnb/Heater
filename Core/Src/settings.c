#include "settings.h"
#include "fan.h"
#include "stm32g0xx_hal.h"
#include <string.h>

#define FLASH_SETTINGS_ADDR 0x0801F800UL
#define SETTINGS_WORDS (sizeof(settings_t) / 8u)   /* three doublewords */

/* Keep the record exactly on a doubleword boundary: HAL_FLASH_Program writes
 * 8 bytes at a time, so a size that is not a multiple of 8 loses the tail. */
typedef char settings_size_check[(sizeof(settings_t) % 8u) ? -1 : 1];

void Settings_Load(settings_t *s) {
    const settings_t *p = (const settings_t *)FLASH_SETTINGS_ADDR;
    if (p->magic == SETTINGS_MAGIC) {
        *s = *p;
    } else {
        s->magic = SETTINGS_MAGIC;
        s->setpoint_x10 = 2000; /* 200.0 C */
        s->iron_pct = 50;       /* defaults the user asked for */
        s->fan_pct = 50;
        s->fan2_pct = 25;
        s->enabled = 1;
        s->fan_auto = 1;
        s->fan2_mute = 0;
        s->kp_x10 = 20;   /* 2.0  */
        s->ki_x100 = 5;   /* 0.05 */
        s->kd_x10 = 10;   /* 1.0  */
    }
    /* sanity-check what came out of flash before it drives the heater */
    /* the floor is 25.0 C: below that the heater is OFF (see ui.h) */
    if (s->setpoint_x10 < 250) s->setpoint_x10 = 250;
    if (s->setpoint_x10 > 5000) s->setpoint_x10 = 2000;
    if (s->iron_pct > 100) s->iron_pct = 50;
    if (s->fan_pct > 100) s->fan_pct = 50;
    if (s->fan2_pct > FAN_CH1_MAX_PCT) s->fan2_pct = FAN_CH1_MAX_PCT;
    if (s->kp_x10 < 0 || s->kp_x10 > 200) s->kp_x10 = 20;
    if (s->ki_x100 < 0 || s->ki_x100 > 500) s->ki_x100 = 5;
    if (s->kd_x10 < 0 || s->kd_x10 > 200) s->kd_x10 = 10;
    s->enabled = s->enabled ? 1 : 0;
    s->fan_auto = s->fan_auto ? 1 : 0;
    s->fan2_mute = s->fan2_mute ? 1 : 0;
    s->magic = SETTINGS_MAGIC;
}

void Settings_Save(const settings_t *s) {
    settings_t t = *s;
    t.magic = SETTINGS_MAGIC;

    /* build the doublewords by copy so the struct never has to be 8 byte
       aligned in RAM, and write all of them */
    uint64_t word[SETTINGS_WORDS];
    for (uint8_t i = 0; i < SETTINGS_WORDS; i++) {
        memcpy(&word[i], (const uint8_t *)&t + i * 8u, 8);
    }

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Page = (FLASH_SETTINGS_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE;
    erase.NbPages = 1;
    uint32_t err = 0;
    if (HAL_FLASHEx_Erase(&erase, &err) == HAL_OK) {
        for (uint8_t i = 0; i < SETTINGS_WORDS; i++) {
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              FLASH_SETTINGS_ADDR + 8u * i, word[i]);
        }
    }
    HAL_FLASH_Lock();
}
