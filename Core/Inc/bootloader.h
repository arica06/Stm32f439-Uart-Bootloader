#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* Call after HAL_Init(), clock and UART init. Never returns:
 * it either jumps to the application or serves firmware updates. */
void bl_run(UART_HandleTypeDef *huart);

/* zlib-compatible CRC32 (same as Python zlib.crc32). Start with crc = 0. */
uint32_t bl_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* BOOTLOADER_H */
