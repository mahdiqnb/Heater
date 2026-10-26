/* fake HAL for the host test bench: only what control.c touches */
#ifndef FAKE_STM32G0XX_HAL_H
#define FAKE_STM32G0XX_HAL_H
#include <stdint.h>
uint32_t HAL_GetTick(void);
#endif
