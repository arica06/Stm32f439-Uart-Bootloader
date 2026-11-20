# STM32F439 UART Bootloader

[![CI](https://github.com/arica06/Stm32f439-Uart-Boot/actions/workflows/ci.yml/badge.svg)](https://github.com/arica06/Stm32f439-Uart-Boot/actions/workflows/ci.yml)

A small, readable UART bootloader for the STM32F439 (intended target: NUCLEO-F439ZI)
with a Python host tool. It validates every image with CRC32, never leaves a
half-written image marked as valid, and keeps the protocol simple enough to
port to other STM32 families.

## Features

- Firmware update over UART with CRC32-protected frames and automatic retry
- Image integrity check (CRC32 over the whole flash image) before it is accepted
- Metadata sector is erased *before* the new image is written, so power loss
  mid-update always results in "no valid application", never a corrupt boot
- Boot-time validation: stack pointer / reset vector sanity + CRC32
- Stay-in-bootloader triggers: user button (B1), RAM flag set by the application, or no valid app
- Only the flash sectors the new image needs are erased
- Host tool in Python with progress output and unit-tested framing code

## Flash layout

| Region      | Address      | Sectors | Size   |
|-------------|--------------|---------|--------|
| Bootloader  | `0x08000000` | 0-2     | 48 KB  |
| Metadata    | `0x0800C000` | 3       | 16 KB  |
| Application | `0x08010000` | 4-11    | 960 KB |
| *(free)*    | `0x08100000` | 12-23   | 1 MB (bank 2, reserved for a future A/B scheme) |

Metadata is four 32-bit words: `magic`, `size`, `crc32`, `version`.

## Boot flow

```
reset
  |
  +-- app valid (metadata + vectors + CRC32)?
  |       no  --> update mode
  |       yes --> B1 held or RAM "stay" flag set?
  |                   yes --> update mode
  |                   no  --> wait 1.5 s for a host frame
  |                               frame received --> update mode
  |                               timeout        --> jump to application
  v
update mode: serve commands until BOOT
```

## Protocol

All multi-byte values are little-endian. Frame format:

```
[0xA5] [CMD] [LEN lo] [LEN hi] [PAYLOAD ...] [CRC32 of CMD..PAYLOAD]
```

CRC32 is the standard zlib polynomial (`zlib.crc32` in Python).

| CMD    | Name  | Payload                                  | Reply payload                              |
|--------|-------|------------------------------------------|--------------------------------------------|
| `0x01` | INFO  | -                                        | version, app address, max size (u32 each) + app-valid (u8) |
| `0x02` | BEGIN | size, crc32, version (u32 each)          | - (erases metadata + needed app sectors)   |
| `0x03` | DATA  | offset (u32) + up to 256 B of data       | -                                          |
| `0x04` | END   | -                                        | - (verifies CRC32, writes metadata)        |
| `0x05` | BOOT  | -                                        | - (jumps to the application)               |

Replies are `0x06` (ACK) or `0x15` (NACK + one error byte: 1 frame, 2 state,
3 param, 4 flash, 5 crc, 6 no app, 7 bad image). DATA chunks must be
sequential and 4-byte aligned; a repeated chunk (lost ACK) is acknowledged
without being written twice.

## Using it in your project

1. Create a STM32CubeIDE project for the NUCLEO-F439ZI (this provides the startup file, linker script, `stm32f4xx_hal_conf.h` and `stm32f4xx_it.c`).
2. Copy `Core/Inc/bootloader.h`, `Core/Inc/bl_config.h`, `Core/Src/bootloader.c` into it.
3. Replace the generated `Core/Src/main.c` with the one in this repo. It sets up the 180 MHz
   clock, USART3 (ST-LINK virtual COM port, 115200 8N1), lights LD2 (blue) while the
   bootloader runs and calls:
   ```c
   bl_run(&huart3);   /* never returns */
   ```
   On another board, adapt the clock and UART setup and keep the `bl_run()` call.
4. Linker script (bootloader): `FLASH ORIGIN = 0x08000000, LENGTH = 48K`, `RAM LENGTH = 191K`
   (the last 16 bytes of SRAM are reserved for the stay flag).
5. Application project:
   - Linker: `FLASH ORIGIN = 0x08010000, LENGTH = 960K`, `RAM LENGTH = 191K`
   - `system_stm32f4xx.c`: `#define VECT_TAB_OFFSET 0x10000`
   - To request an update from the running application:
     ```c
     *(volatile uint32_t *)0x2002FFF0 = 0xB00710AD;
     NVIC_SystemReset();
     ```

## Flashing an application

```bash
pip install pyserial
python tools/flash_tool.py COM5 app.bin --version 1 --boot     # Windows
python tools/flash_tool.py /dev/ttyACM0 app.bin --boot          # Linux
```

Start the tool, then press RESET on the board. The tool keeps sending INFO
requests until the bootloader answers, then erases, writes, verifies and
(with `--boot`) starts the application.

## Tests and CI

- `tests/` - unit tests for the host tool's framing and CRC handling (`pytest -q tests`)
- GitHub Actions runs the host tests and a Cortex-M4 compile check of the
  bootloader against the ST HAL and CMSIS headers (warnings enabled)

## Limitations and roadmap

- Integrity only (CRC32), no authenticity: no signature check yet. The F439 HASH peripheral can accelerate SHA-256; an ECDSA/Ed25519 check is the natural next step.
- UART in polling mode; higher baud rates need DMA with idle-line detection.
- Single image slot. Bank 2 is reserved for an A/B scheme with rollback.
- The boot-time CRC is a software CRC over the whole image, which costs boot time on large images.

## License

This project is licensed under the [MIT License](LICENSE).
