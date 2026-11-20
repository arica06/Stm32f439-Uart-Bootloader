#include "bootloader.h"
#include "bl_config.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc;
    uint32_t version;
} bl_meta_t;

typedef struct {
    bool     active;
    uint32_t size;
    uint32_t crc;
    uint32_t version;
    uint32_t next;      /* next expected byte offset */
} xfer_t;

static UART_HandleTypeDef *g_uart;
static uint8_t g_rx[3u + BL_MAX_PAYLOAD + 4u];
static xfer_t  g_xfer;
static bool    g_app_valid;

/* ------------------------------------------------------------------ CRC32 */

uint32_t bl_crc32(uint32_t crc, const uint8_t *d, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* ------------------------------------------------------------ validation  */

static bool sp_ok(uint32_t sp)
{
    return (sp >= 0x20000000u && sp <= 0x20030000u) ||   /* SRAM1-3 */
           (sp >= 0x10000000u && sp <= 0x10010000u);     /* CCM     */
}

static bool vectors_ok(void)
{
    uint32_t sp = *(volatile uint32_t *)(BL_APP_ADDR);
    uint32_t pc = *(volatile uint32_t *)(BL_APP_ADDR + 4u);
    return sp_ok(sp) && pc >= BL_APP_ADDR && pc < BL_APP_END && (pc & 1u);
}

static bool app_valid(void)
{
    const bl_meta_t *m = (const bl_meta_t *)BL_META_ADDR;
    if (m->magic != BL_META_MAGIC) return false;
    if (m->size < 8u || m->size > BL_APP_MAX_SIZE) return false;
    if (!vectors_ok()) return false;
    return bl_crc32(0, (const uint8_t *)BL_APP_ADDR, m->size) == m->crc;
}

/* ------------------------------------------------------------------ flash */

static void flash_clear_flags(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

/* Erase metadata first (so a half-written image is never "valid"),
 * then only the application sectors the new image needs. */
static bool flash_erase_app(uint32_t size)
{
    uint32_t last_addr   = BL_APP_ADDR + size - 1u;
    uint32_t last_sector = (last_addr < BL_SECTOR5_ADDR)
                         ? BL_APP_FIRST_SECTOR
                         : 5u + (last_addr - BL_SECTOR5_ADDR) / BL_BIG_SECTOR_SIZE;

    FLASH_EraseInitTypeDef e = {0};
    uint32_t err = 0;
    bool ok;

    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;      /* 2.7-3.6 V, word programming */

    HAL_FLASH_Unlock();
    flash_clear_flags();

    e.Sector    = BL_META_SECTOR;
    e.NbSectors = 1;
    ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);

    if (ok) {
        e.Sector    = BL_APP_FIRST_SECTOR;
        e.NbSectors = last_sector - BL_APP_FIRST_SECTOR + 1u;
        ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);
    }

    HAL_FLASH_Lock();
    return ok;
}

static bool flash_write(uint32_t addr, const uint8_t *d, uint32_t n)
{
    bool ok = true;

    HAL_FLASH_Unlock();
    flash_clear_flags();
    for (uint32_t i = 0; i < n; i += 4u) {
        uint32_t w;
        memcpy(&w, d + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) {
            ok = false;
            break;
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

/* ---------------------------------------------------------------- framing */
/* Frame: [SOF][CMD][LEN lo][LEN hi][PAYLOAD...][CRC32 LE over CMD..PAYLOAD] */

static void send_frame(uint8_t cmd, const void *p, uint16_t n)
{
    uint8_t tx[1u + 3u + 16u + 4u];
    uint32_t crc;

    if (n > 16u) return;
    tx[0] = BL_SOF;
    tx[1] = cmd;
    tx[2] = (uint8_t)(n & 0xFFu);
    tx[3] = (uint8_t)(n >> 8);
    if (n) memcpy(&tx[4], p, n);
    crc = bl_crc32(0, &tx[1], 3u + n);
    memcpy(&tx[4u + n], &crc, 4);
    HAL_UART_Transmit(g_uart, tx, 1u + 3u + n + 4u, 100);
}

static void ack(void)             { send_frame(BL_ACK, NULL, 0); }
static void nack(uint8_t code)    { send_frame(BL_NACK, &code, 1); }

/* 0 = valid frame, -1 = timeout waiting for SOF, -2 = malformed/bad CRC */
static int recv_frame(uint32_t timeout_ms, uint8_t *cmd, const uint8_t **payload, uint16_t *len)
{
    uint8_t  b = 0;
    uint32_t t0 = HAL_GetTick();
    bool     got_sof = false;

    while (HAL_GetTick() - t0 < timeout_ms) {
        if (HAL_UART_Receive(g_uart, &b, 1, 10) == HAL_OK && b == BL_SOF) {
            got_sof = true;
            break;
        }
    }
    if (!got_sof) return -1;

    if (HAL_UART_Receive(g_uart, g_rx, 3, 100) != HAL_OK) return -2;

    uint16_t n = (uint16_t)(g_rx[1] | (g_rx[2] << 8));
    if (n > BL_MAX_PAYLOAD) return -2;
    if (HAL_UART_Receive(g_uart, &g_rx[3], n + 4u, 500) != HAL_OK) return -2;

    uint32_t rx_crc;
    memcpy(&rx_crc, &g_rx[3u + n], 4);
    if (bl_crc32(0, g_rx, 3u + n) != rx_crc) return -2;

    *cmd     = g_rx[0];
    *payload = &g_rx[3];
    *len     = n;
    return 0;
}

/* ---------------------------------------------------------------- jump    */

static void jump_to_app(void)
{
    uint32_t sp = *(volatile uint32_t *)(BL_APP_ADDR);
    uint32_t pc = *(volatile uint32_t *)(BL_APP_ADDR + 4u);

    HAL_UART_DeInit(g_uart);
    HAL_DeInit();
    HAL_RCC_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    SCB->VTOR = BL_APP_ADDR;
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) { }
}

/* ---------------------------------------------------------------- commands */

static void dispatch(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    switch (cmd) {

    case BL_CMD_INFO: {
        uint8_t  r[13];
        uint32_t v = BL_VERSION, a = BL_APP_ADDR, m = BL_APP_MAX_SIZE;
        memcpy(&r[0], &v, 4);
        memcpy(&r[4], &a, 4);
        memcpy(&r[8], &m, 4);
        r[12] = g_app_valid ? 1u : 0u;
        send_frame(BL_ACK, r, sizeof r);
        break;
    }

    case BL_CMD_BEGIN: {
        uint32_t size, crc, ver;
        if (n != 12u) { nack(BL_ERR_PARAM); break; }
        memcpy(&size, &p[0], 4);
        memcpy(&crc,  &p[4], 4);
        memcpy(&ver,  &p[8], 4);
        if (size < 8u || size > BL_APP_MAX_SIZE || (size & 3u)) { nack(BL_ERR_PARAM); break; }

        g_xfer.active = false;
        g_app_valid   = false;
        if (!flash_erase_app(size)) { nack(BL_ERR_FLASH); break; }

        g_xfer.active  = true;
        g_xfer.size    = size;
        g_xfer.crc     = crc;
        g_xfer.version = ver;
        g_xfer.next    = 0;
        ack();
        break;
    }

    case BL_CMD_DATA: {
        uint32_t off, len;
        if (!g_xfer.active)              { nack(BL_ERR_STATE); break; }
        if (n < 8u || ((n - 4u) & 3u))   { nack(BL_ERR_PARAM); break; }
        memcpy(&off, &p[0], 4);
        len = n - 4u;
        if (off + len <= g_xfer.next)    { ack(); break; }          /* duplicate (lost ACK) */
        if (off != g_xfer.next || off + len > g_xfer.size) { nack(BL_ERR_PARAM); break; }
        if (!flash_write(BL_APP_ADDR + off, &p[4], len))   { nack(BL_ERR_FLASH); break; }
        g_xfer.next += len;
        ack();
        break;
    }

    case BL_CMD_END: {
        bl_meta_t m;
        if (!g_xfer.active || g_xfer.next != g_xfer.size) { nack(BL_ERR_STATE); break; }
        if (bl_crc32(0, (const uint8_t *)BL_APP_ADDR, g_xfer.size) != g_xfer.crc) {
            nack(BL_ERR_CRC);
            break;
        }
        if (!vectors_ok()) { nack(BL_ERR_IMAGE); break; }

        m.magic   = BL_META_MAGIC;
        m.size    = g_xfer.size;
        m.crc     = g_xfer.crc;
        m.version = g_xfer.version;
        if (!flash_write(BL_META_ADDR, (const uint8_t *)&m, sizeof m)) { nack(BL_ERR_FLASH); break; }

        g_xfer.active = false;
        g_app_valid   = app_valid();
        if (g_app_valid) ack(); else nack(BL_ERR_IMAGE);
        break;
    }

    case BL_CMD_BOOT:
        if (!g_app_valid) { nack(BL_ERR_NOAPP); break; }
        ack();
        jump_to_app();
        break;

    default:
        nack(BL_ERR_PARAM);
        break;
    }
}

/* ------------------------------------------------------------------ entry */

static bool stay_requested(void)
{
    volatile uint32_t *flag = (volatile uint32_t *)BL_STAY_FLAG_ADDR;
    bool stay = (*flag == BL_STAY_MAGIC);
    *flag = 0;

    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    g.Pin  = BL_BUTTON_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(BL_BUTTON_PORT, &g);
    if (HAL_GPIO_ReadPin(BL_BUTTON_PORT, BL_BUTTON_PIN) == GPIO_PIN_SET) stay = true;

    return stay;
}

void bl_run(UART_HandleTypeDef *huart)
{
    uint8_t cmd;
    const uint8_t *payload;
    uint16_t len;

    g_uart      = huart;
    g_app_valid = app_valid();
    bool stay   = stay_requested();      /* also clears the RAM flag */

    if (g_app_valid && !stay) {
        /* Give the host a short window to interrupt the boot. */
        bool host_found = false;
        uint32_t t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < BL_LISTEN_MS) {
            if (recv_frame(BL_LISTEN_MS, &cmd, &payload, &len) == 0) {
                dispatch(cmd, payload, len);
                host_found = true;
                break;
            }
        }
        if (!host_found) jump_to_app();
    }

    for (;;) {
        int rc = recv_frame(1000, &cmd, &payload, &len);
        if (rc == 0)       dispatch(cmd, payload, len);
        else if (rc == -2) nack(BL_ERR_FRAME);
    }
}
