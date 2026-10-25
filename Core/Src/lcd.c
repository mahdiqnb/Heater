#include "lcd.h"
#include "font5x7.h"
#include "stm32g0xx_hal.h"

/* ---- low-level bitbang SPI ---- */
static void spi_write_byte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET); /* SCK low */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, (b & 0x80) ? GPIO_PIN_SET : GPIO_PIN_RESET); /* MOSI */
        b <<= 1;
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET); /* SCK high */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET); /* SCK low */
    }
}

/* ILI9341 command/data */
static void lcd_cmd(uint8_t c) {
    HAL_GPIO_WritePin(D_C_GPIO_Port, D_C_Pin, GPIO_PIN_RESET); /* DC low = command */
    HAL_GPIO_WritePin(RESET_GPIO_Port, RESET_Pin, GPIO_PIN_SET); /* keep reset high */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET); /* CS low */
    spi_write_byte(c);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET); /* CS high */
}
static void lcd_data(uint8_t d) {
    HAL_GPIO_WritePin(D_C_GPIO_Port, D_C_Pin, GPIO_PIN_SET); /* DC high = data */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
    spi_write_byte(d);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
}
static void lcd_data16(uint16_t d) {
    lcd_data(d >> 8);
    lcd_data(d & 0xFF);
}

/* ---- init ---- */
void LCD_Init(void) {
    /* reset */
    HAL_GPIO_WritePin(RESET_GPIO_Port, RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(RESET_GPIO_Port, RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(150);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET); /* CS high */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET); /* SCK low */

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
    lcd_cmd(0x26);
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

    /* test: fill red to confirm display works */
    LCD_Fill(0xF800);
    HAL_Delay(300);
    LCD_Fill(C_BLACK);
}

/* ---- drawing ---- */
void LCD_Fill(uint16_t color) {
    LCD_FillRect(0, 0, LCD_W, LCD_H, color);
}

void LCD_SetWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    lcd_cmd(0x2A); /* column addr */
    lcd_data(x0 >> 8); lcd_data(x0 & 0xFF);
    lcd_data(x1 >> 8); lcd_data(x1 & 0xFF);
    lcd_cmd(0x2B); /* page addr */
    lcd_data(y0 >> 8); lcd_data(y0 & 0xFF);
    lcd_data(y1 >> 8); lcd_data(y1 & 0xFF);
    lcd_cmd(0x2C); /* memory write */
}

void LCD_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x >= LCD_W || y >= LCD_H) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    HAL_GPIO_WritePin(D_C_GPIO_Port, D_C_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
    for (int i = 0; i < w * h; i++) {
        spi_write_byte(color >> 8);
        spi_write_byte(color & 0xFF);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
}

void LCD_DrawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
    LCD_SetWindow(x, y, x, y);
    HAL_GPIO_WritePin(D_C_GPIO_Port, D_C_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
    spi_write_byte(color >> 8);
    spi_write_byte(color & 0xFF);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
}

void LCD_DrawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    int16_t w = 6 * scale;
    int16_t h = 7 * scale;
    if (x + w > LCD_W || y + h > LCD_H) return;
    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    HAL_GPIO_WritePin(D_C_GPIO_Port, D_C_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int sy = 0; sy < scale; sy++) {
            for (int col = 0; col < 5; col++) {
                uint16_t color = (bits & (1 << (4 - col))) ? fg : bg;
                for (int sx = 0; sx < scale; sx++) {
                    spi_write_byte(color >> 8);
                    spi_write_byte(color & 0xFF);
                }
            }
            /* spacing column */
            for (int sx = 0; sx < scale; sx++) {
                spi_write_byte(bg >> 8);
                spi_write_byte(bg & 0xFF);
            }
        }
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
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