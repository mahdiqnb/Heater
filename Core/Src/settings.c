#include "settings.h"
#include "stm32g0xx_hal.h"

#define FLASH_SETTINGS_ADDR 0x0801F800UL
#define SETTINGS_MAGIC 0x48454154UL /* "HEAT" */

void Settings_Load(settings_t *s) {
    settings_t *p = (settings_t *)FLASH_SETTINGS_ADDR;
    if (p->magic == SETTINGS_MAGIC) {
        *s = *p;
    } else {
        s->magic = SETTINGS_MAGIC;
        s->setpoint_x10 = 2000; /* 200.0 C */
        s->iron_pct = 0;
        s->fan_pct = 0;
        s->enabled = 1;
    }
}

void Settings_Save(const settings_t *s) {
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Page = (FLASH_SETTINGS_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE;
    erase.NbPages = 1;
    uint32_t err = 0;
    if (HAL_FLASHEx_Erase(&erase, &err) == HAL_OK) {
        /* program as doubleword */
        uint64_t *data = (uint64_t *)s;
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FLASH_SETTINGS_ADDR, data[0]);
    }
    HAL_FLASH_Lock();
}