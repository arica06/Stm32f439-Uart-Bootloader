# STM32F439 UART Bootloader

[![CI](https://github.com/arica06/Stm32f439-Uart-Bootloader/actions/workflows/ci.yml/badge.svg)](https://github.com/arica06/Stm32f439-Uart-Bootloader/actions/workflows/ci.yml)

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

The project configuration, linker scripts, and startup file are in the repository root:

| File | Purpose |
|------|---------|
| [STM32F439_Bootloader.ioc](STM32F439_Bootloader.ioc) | STM32CubeMX / STM32CubeIDE configuration for STM32F439ZI |
| [STM32F439ZI_FLASH_BL.ld](STM32F439ZI_FLASH_BL.ld) | Bootloader linker script: 48 KB flash at `0x08000000` |
| [STM32F439ZI_FLASH_APP.ld](STM32F439ZI_FLASH_APP.ld) | Application linker script: 960 KB flash at `0x08010000` |
| [startup_stm32f439xx.s](startup_stm32f439xx.s) | GNU assembler startup code and STM32F439xx interrupt vector table |

1. Open `STM32F439_Bootloader.ioc` in STM32CubeMX / STM32CubeIDE and generate
   the STM32CubeIDE project and HAL/CMSIS support files. Keep the generated
   `system_stm32f4xx.c`, `stm32f4xx_hal_conf.h`, and `stm32f4xx_it.c`.
   Ensure the HAL UART module is enabled in `stm32f4xx_hal_conf.h` and its driver
   sources are included in the build; the supplied `main.c` configures USART3
   and its GPIO pins directly.
2. Copy `Core/Inc/bootloader.h`, `Core/Inc/bl_config.h`, and
   `Core/Src/bootloader.c` into the generated project. Use this repository's
   `Core/Src/main.c` as the project's main file. It configures the 180 MHz clock,
   USART3 (PD8 TX / PD9 RX, ST-LINK virtual COM port, 115200 8N1), and LD2 (blue),
   then calls:
   ```c
   bl_run(&huart3);   /* never returns */
   ```
   On another board, adapt the clock and UART setup and keep the `bl_run()` call.
   Keep `SysTick_Handler()` calling `HAL_IncTick()` in `stm32f4xx_it.c`.
3. Add the root-level `startup_stm32f439xx.s` to the build. Replace or exclude
   the generated startup file so that exactly one startup file is compiled.
   Keep the generated interrupt handlers; they override the startup file's weak
   defaults.
4. In **Project Properties → C/C++ Build → Settings → MCU GCC Linker → General**,
   select `STM32F439ZI_FLASH_BL.ld` from the project root as the bootloader's
   linker script. Replace the generated linker-script selection rather than
   adding a second script. The supplied script sets flash to
   `ORIGIN = 0x08000000, LENGTH = 48K`, RAM to 191 KB, and CCMRAM to 64 KB.
5. In a separate application project, select `STM32F439ZI_FLASH_APP.ld` as the
   linker script. It places the application at
   `ORIGIN = 0x08010000, LENGTH = 960K` and uses the same RAM reservation.
   In the application's `system_stm32f4xx.c`, set
   `#define VECT_TAB_OFFSET 0x10000`; enable `USER_VECT_TAB_ADDRESS` if required
   by that CMSIS system-file version so the offset is applied.
6. Both linker scripts reserve the final 1 KB of SRAM
   (`0x2002FC00..0x2002FFFF`), including the stay-in-bootloader flag at
   `0x2002FFF0`. To request an update from the running application:
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
