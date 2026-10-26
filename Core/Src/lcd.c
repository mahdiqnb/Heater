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
#define CS_CLR()   ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_3 << 16)
#define CS_SET()   ((GPIOA)->BSRR = GPIO_PIN_3)
#define DC_CLR()   ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_5 << 16)
#define DC_SET()   ((GPIOA)->BSRR = GPIO_PIN_5)
#define RST_CLR()  ((GPIOA)->BSRR = (uint32_t)GPIO_PIN_4 << 16)
#define RST_SET()  ((GPIOA)->BSRR = GPIO_PIN_4)

static inline void spi_tx(uint8_t b) {
    while ((SPI1->SR & SPI_SR_TXE) == 0U) { }
    *(volatile uint8_t *)&SPI1->DR = b;
}

/* wait for the last clocked-out bit to leave the shift register */
static inline void spi_done(void) {
    while (SPI1->SR & SPI_SR_BSY) { }
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

    /* CS / RESET / D-C as plain outputs */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &g);

    /* SCK + MOSI on SPI1, 8 MHz (16 MHz / 2). If long wiring shows
       artefacts, change this to SPI_BAUDRATEPRESCALER_4 (4 MHz). */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF0_SPI1;
    g.Pin = GPIO_PIN_1 | GPIO_PIN_2;
    HAL_GPIO_Init(GPIOA, &g);

    /* Every field is set here on purpose. MX_SPI1_Init() (CubeMX generated)
       comes out with SPI_DATASIZE_4BIT because the .ioc carries no DataSize
       key, and with 4-bit frames the ILI9341 receives garbage for every
       command -- the panel then stays blank/white. Overriding the whole
       structure in user code also survives the next CubeMX regenerate. */
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    HAL_SPI_Init(&hspi1);   /* rewrites CR1/CR2 and disables the peripheral */
    __HAL_SPI_CLEAR_OVRFLAG(&hspi1);
    __HAL_SPI_ENABLE(&hspi1);

    CS_SET();

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
