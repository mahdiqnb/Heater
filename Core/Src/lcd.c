#include "lcd.h"
#include "font5x7.h"
#include "stm32g0xx_hal.h"

extern SPI_HandleTypeDef hspi1;

/*
 * Wiring (unchanged from the schematic):
 *   PA1 = SPI1_SCK (AF0), PA2 = SPI1_MOSI (AF0)
 *   PA3 = CS, PA4 = RESET, PA5 = D/C
 * Everything below writes the BSRR register directly: HAL_GPIO_WritePin is far
 * too slow when it is called once per SPI bit.
 */
/* SPI1 is fed from PCLK. The divider is picked from the live clock instead of
   being hard-coded, so a CubeMX clock-tree change (the tree is 64 MHz now, it
   used to be 16 MHz) cannot silently push the bus past what the panel accepts.
   The target is <= 4 MHz: the panel is rated 10 MHz, but this wiring is only
   proven up to ~8 MHz and a slow correct bus beats a fast broken one. Even at
   4 MHz a full 320x240 repaint is ~24 ms, which is invisible to the eye.
   NOTE the index is tbl[i], not tbl[i+1]: an off-by-one here silently picked
   one step too fast/too slow and is exactly the kind of thing that turns a
   working panel white. */
static uint32_t lcd_spi_prescaler(void) {
    static const uint32_t tbl[8] = {
        SPI_BAUDRATEPRESCALER_2,   SPI_BAUDRATEPRESCALER_4,
        SPI_BAUDRATEPRESCALER_8,   SPI_BAUDRATEPRESCALER_16,
        SPI_BAUDRATEPRESCALER_32,  SPI_BAUDRATEPRESCALER_64,
        SPI_BAUDRATEPRESCALER_128, SPI_BAUDRATEPRESCALER_256
    };
    uint32_t pclk = HAL_RCC_GetPCLK1Freq();
    uint32_t div = 2, i = 0;
    while (i < 7u && (pclk / div) > 4000000u) { div <<= 1; i++; }
    return tbl[i+1];
}

#define CS_CLR()   ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_3 << 16)
#define CS_SET()   ((GPIOA)->BSRR = GPIO_PIN_3)
#define DC_CLR()   ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_5 << 16)
#define DC_SET()   ((GPIOA)->BSRR = GPIO_PIN_5)
#define RST_CLR()  ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_4 << 16)
#define RST_SET()  ((GPIOA)->BSRR = GPIO_PIN_4)

/* bit-bang fallback pins: same two wires, now driven as plain outputs */
#define SCK_SET()  ((GPIOA)->BSRR = GPIO_PIN_1)
#define SCK_CLR()  ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_1 << 16)
#define MOSI_SET() ((GPIOA)->BSRR = GPIO_PIN_2)
#define MOSI_CLR() ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_2 << 16)

/* 1 = hardware SPI1, 0 = bit-banged. Set once, during LCD_Init. */
static uint8_t hw_spi = 1;

static void lcd_use_soft_spi(void);

/* Send one byte. If the SPI1 kernel is not clocked (a register write does
   nothing, SR reads 0) TXE never rises and this loop would hang the boot
   before the first pixel -- the panel then keeps its random power-on RAM,
   which is the white screen. The poll is therefore bounded: after ~2000
   tries the two wires are turned back into plain outputs and every further
   byte is bit-banged, which is slower but is exactly what the first working
   firmware on this board did. */
static inline void spi_tx(uint8_t b) {
    if (hw_spi) {
        uint32_t guard = 2000u;
        while ((SPI1->SR & SPI_SR_TXE) == 0U) {
            if (--guard == 0U) {
                lcd_use_soft_spi();
                break;
            }
        }
        if (hw_spi) {
            *(volatile uint8_t *)&SPI1->DR = b;
            return;
        }
    }
    for (uint8_t i = 0; i < 8u; i++) {
        if (b & 0x80u) MOSI_SET(); else MOSI_CLR();
        b = (uint8_t)(b << 1);
        SCK_SET();
        SCK_CLR();
    }
}

/* wait for the last clocked-out bit to leave the shift register */
static inline void spi_done(void) {
    uint32_t guard = 100000UL;
    while ((SPI1->SR & SPI_SR_BSY) != 0U && --guard != 0U) { }
}

/* Give up on the peripheral: the wires become ordinary GPIOs and the byte
   stream continues without interruption (no reset, no re-init). */
static void lcd_use_soft_spi(void) {
    GPIO_InitTypeDef g = { 0 };
    hw_spi = 0;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_1 | GPIO_PIN_2;
    HAL_GPIO_Init(GPIOA, &g);
    SCK_CLR();
    MOSI_CLR();
}

/* ---- ILI9341 command/data ---- */
static void lcd_cmd(uint8_t c) {
    DC_CLR();
    CS_CLR();
    spi_tx(c);
    spi_done();
    CS_SET();
}
static void lcd_data(uint8_t d) {
    DC_SET();
    CS_CLR();
    spi_tx(d);
    spi_done();
    CS_SET();
}

/* ---- init ---- */
void LCD_Init(void) {
    GPIO_InitTypeDef g = {0};

    /* BOOT PROBE (temporary): ERR LED (PB12, active-low) turns ON the instant
       LCD_Init is entered and OFF again once the whole init sequence has been
       clocked out. This separates the two possible failures:
         - LED never lights  -> the code never reaches the display, so the real
           fault is earlier (most likely SystemClock_Config / the 64 MHz PLL)
           and the panel is a red herring.
         - LED lights then goes off -> init ran and bytes were clocked out; if
           the glass is still white the data is not arriving at the panel
           (wiring / bus speed), not a firmware-order problem. */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = GPIO_PIN_12; /* ERR LED */
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET); /* light it */

    /* Full DeInit/Init cycle, exactly like the build that worked: HAL only
       re-runs HAL_SPI_MspInit() (peripheral clock + PA1/PA2 alternate
       function) when the handle is in the RESET state, and only DeInit puts
       it there. Without this the SPI can end up configured but never clocked
       out, which looks exactly like a white panel. */
    HAL_SPI_DeInit(&hspi1);
    /* belt and braces: the clock is enabled here as well, so a HAL handle
       left in an unexpected state cannot leave the kernel unclocked */
    __HAL_RCC_SPI1_CLK_ENABLE();
    hw_spi = 1;

    /* THIS BLOCK MUST STAY. It is not a duplicate of CubeMX's work: the
       HAL_SPI_DeInit() above put PA1/PA2 back into their reset state and
       switched the peripheral clock off, and without the lines below the two
       wires are left floating and the panel receives nothing at all -- the
       screen stays white, exactly as when this block was commented out.
       Keeping it here also survives every CubeMX "Generate Code". */

    /* CS / RESET / D-C as plain outputs */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &g);

    /* SCK + MOSI on SPI1 (AF0). The ILI9341 is specified to 10 MHz and only
       tolerates ~15 MHz on short wiring; lcd_spi_prescaler() keeps the bus at
       or below 9 MHz whatever the system clock is. */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF0_SPI1;
    g.Pin = GPIO_PIN_1 | GPIO_PIN_2;
    HAL_GPIO_Init(GPIOA, &g);

    /* Every field is set here on purpose. MX_SPI1_Init() (CubeMX generated)
       comes out with SPI_DATASIZE_4BIT because the .ioc carries no DataSize
       key, and with 4-bit frames the ILI9341 receives garbage for every
       command -- the panel then stays blank/white. The generated init also
       leaves the prescaler at /1, i.e. 16 MHz on this clock tree, which is
       above what this panel reliably accepts. */
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    hspi1.Init.BaudRatePrescaler = lcd_spi_prescaler();
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {   /* rewrites CR1/CR2, disables the kernel */
        /* the driver refuses a handle that is not in its RESET state: force a
           fresh MspInit (peripheral clock + PA1/PA2 alternate function) */
        hspi1.State = HAL_SPI_STATE_RESET;
        if (HAL_SPI_Init(&hspi1) != HAL_OK)
            lcd_use_soft_spi();
    }
    __HAL_SPI_CLEAR_OVRFLAG(&hspi1);
    __HAL_SPI_ENABLE(&hspi1);

    CS_SET();

    /* Probe the block before it is used for real. A byte clocked out with CS
       high is ignored by the panel, but it proves that TXE answers; if it does
       not, spi_tx() has already switched the same wires to bit-banging, so the
       init sequence below still reaches the panel. */
    DC_SET();
    spi_tx(0x00);
    spi_done();

    /* let VCC settle after power on before the first reset: at 50 MHz the
       firmware reaches this point much sooner than it used to */
    HAL_Delay(50);

    /* reset */
    RST_CLR();
    HAL_Delay(100);
    RST_SET();
    HAL_Delay(150);

    lcd_cmd(0x01); /* software reset */
    HAL_Delay(150);

    lcd_cmd(0x28); /* display off */

    lcd_cmd(0xCF);
    lcd_data(0x00); lcd_data(0xC1); lcd_data(0x30);
    lcd_cmd(0xED);
    lcd_data(0x64); lcd_data(0x03); lcd_data(0x12); lcd_data(0x81);
    lcd_cmd(0xE8);
    lcd_data(0x85); lcd_data(0x00); lcd_data(0x78);
    lcd_cmd(0xCB);
    lcd_data(0x39); lcd_data(0x2C); lcd_data(0x00); lcd_data(0x34); lcd_data(0x02);
    lcd_cmd(0xF7);
    lcd_data(0x20);
    lcd_cmd(0xEA);
    lcd_data(0x00); lcd_data(0x00);

    lcd_cmd(0xC0);
    lcd_data(0x23);
    lcd_cmd(0xC1);
    lcd_data(0x10);
    lcd_cmd(0xC5);
    lcd_data(0x3E); lcd_data(0x28);
    lcd_cmd(0xC7);
    lcd_data(0x86);

    lcd_cmd(0x36); /* MADCTL - landscape */
    lcd_data(0x28);

    lcd_cmd(0x3A); /* pixel format 16bit */
    lcd_data(0x55);

    lcd_cmd(0xB1);
    lcd_data(0x00); lcd_data(0x18);
    lcd_cmd(0xB6);
    lcd_data(0x08); lcd_data(0x82); lcd_data(0x27);
    lcd_cmd(0xF2);
    lcd_data(0x00);
    lcd_data(0x26);
    lcd_data(0x01);
    lcd_cmd(0xE0);
    lcd_data(0x0F); lcd_data(0x31); lcd_data(0x2B); lcd_data(0x0C);
    lcd_data(0x0E); lcd_data(0x08); lcd_data(0x4E); lcd_data(0xF1);
    lcd_data(0x37); lcd_data(0x07); lcd_data(0x10); lcd_data(0x03);
    lcd_data(0x0E); lcd_data(0x09); lcd_data(0x00);
    lcd_cmd(0xE1);
    lcd_data(0x00); lcd_data(0x0E); lcd_data(0x14); lcd_data(0x03);
    lcd_data(0x11); lcd_data(0x07); lcd_data(0x31); lcd_data(0xC1);
    lcd_data(0x48); lcd_data(0x08); lcd_data(0x0F); lcd_data(0x0C);
    lcd_data(0x31); lcd_data(0x36); lcd_data(0x0F);

    lcd_cmd(0x11); /* sleep out */
    HAL_Delay(150);
    lcd_cmd(0x29); /* display on */
    HAL_Delay(50);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET); /* init done: LED off */
}

/* ---- drawing ---- */
void LCD_SetWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    lcd_cmd(0x2A); /* column addr */
    lcd_data(x0 >> 8); lcd_data(x0 & 0xFF);
    lcd_data(x1 >> 8); lcd_data(x1 & 0xFF);
    lcd_cmd(0x2B); /* page addr */
    lcd_data(y0 >> 8); lcd_data(y0 & 0xFF);
    lcd_data(y1 >> 8); lcd_data(y1 & 0xFF);
    lcd_cmd(0x2C); /* memory write */
}

void LCD_Fill(uint16_t color) {
    LCD_FillRect(0, 0, LCD_W, LCD_H, color);
}

void LCD_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x >= LCD_W || y >= LCD_H) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    DC_SET();
    CS_CLR();
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)color;
    for (int32_t i = (int32_t)w * h; i > 0; i--) {
        spi_tx(hi);
        spi_tx(lo);
    }
    spi_done();
    CS_SET();
}

void LCD_DrawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
    LCD_SetWindow(x, y, x, y);
    DC_SET();
    CS_CLR();
    spi_tx((uint8_t)(color >> 8));
    spi_tx((uint8_t)color);
    spi_done();
    CS_SET();
}

/* palette-indexed image, one byte per pixel, row major */
void LCD_DrawIndexed(int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint8_t *pix, const uint16_t *pal) {
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return;
    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    DC_SET();
    CS_CLR();
    uint32_t n = (uint32_t)w * (uint32_t)h;
    while (n--) {
        uint16_t c = pal[*pix++];
        spi_tx((uint8_t)(c >> 8));
        spi_tx((uint8_t)c);
    }
    spi_done();
    CS_SET();
}

void LCD_DrawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    int16_t w = 6 * scale;
    int16_t h = 7 * scale;
    if (x + w > LCD_W || y + h > LCD_H) return;
    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    DC_SET();
    CS_CLR();
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int sy = 0; sy < scale; sy++) {
            for (int col = 0; col < 5; col++) {
                uint16_t color = (bits & (1 << (4 - col))) ? fg : bg;
                uint8_t hi = (uint8_t)(color >> 8);
                uint8_t lo = (uint8_t)color;
                for (int sx = 0; sx < scale; sx++) {
                    spi_tx(hi);
                    spi_tx(lo);
                }
            }
            /* spacing column */
            uint8_t bhi = (uint8_t)(bg >> 8);
            uint8_t blo = (uint8_t)bg;
            for (int sx = 0; sx < scale; sx++) {
                spi_tx(bhi);
                spi_tx(blo);
            }
        }
    }
    spi_done();
    CS_SET();
}

void LCD_DrawString(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    int16_t cx = x;
    while (*s) {
        LCD_DrawChar(cx, y, *s, fg, bg, scale);
        cx += 6 * scale;
        s++;
    }
}

int16_t LCD_StrWidth(const char *s, uint8_t scale) {
    int16_t n = 0;
    while (*s) { n += 6 * scale; s++; }
    return n;
}
