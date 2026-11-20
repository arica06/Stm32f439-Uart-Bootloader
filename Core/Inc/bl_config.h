#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/*
 * STM32F439 flash map (bank 1 only, single-image bootloader)
 *
 *   0x08000000  sectors 0-2   48 KB   bootloader
 *   0x0800C000  sector  3     16 KB   metadata (magic, size, crc32, version)
 *   0x08010000  sectors 4-11  960 KB  application
 *
 * Bank 2 (0x08100000..) is left free for a later A/B scheme.
 */

#define BL_VERSION            0x00010000u

#define BL_APP_ADDR           0x08010000u
#define BL_APP_END            0x08100000u            /* exclusive */
#define BL_APP_MAX_SIZE       (BL_APP_END - BL_APP_ADDR)

#define BL_META_ADDR          0x0800C000u
#define BL_META_SECTOR        FLASH_SECTOR_3
#define BL_APP_FIRST_SECTOR   4u                     /* 64 KB sector, then 128 KB each */
#define BL_SECTOR5_ADDR       0x08020000u
#define BL_BIG_SECTOR_SIZE    0x20000u

#define BL_META_MAGIC         0xB007AB1Eu

/* "Stay in bootloader" request: the application writes this magic to the last
 * 16 bytes of SRAM and resets. Reserve that RAM in BOTH linker scripts. */
#define BL_STAY_FLAG_ADDR     0x2002FFF0u
#define BL_STAY_MAGIC         0xB00710ADu

/* Nucleo-144 user button B1 (PC13, active high). Hold during reset to stay. */
#define BL_BUTTON_PORT        GPIOC
#define BL_BUTTON_PIN         GPIO_PIN_13

#define BL_LISTEN_MS          1500u                  /* window to catch the host after reset */

/* Wire protocol */
#define BL_SOF                0xA5u
#define BL_CHUNK              256u
#define BL_MAX_PAYLOAD        (BL_CHUNK + 4u)

#define BL_CMD_INFO           0x01u
#define BL_CMD_BEGIN          0x02u
#define BL_CMD_DATA           0x03u
#define BL_CMD_END            0x04u
#define BL_CMD_BOOT           0x05u
#define BL_ACK                0x06u
#define BL_NACK               0x15u

#define BL_ERR_FRAME          1u
#define BL_ERR_STATE          2u
#define BL_ERR_PARAM          3u
#define BL_ERR_FLASH          4u
#define BL_ERR_CRC            5u
#define BL_ERR_NOAPP          6u
#define BL_ERR_IMAGE          7u

#endif /* BL_CONFIG_H */
