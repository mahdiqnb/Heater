#ifndef _LCD_H_
#define _LCD_H_
#include "main.h"
#include <stdint.h>

/* ILI9341 2.2" TFT, landscape 320x240 */
#define LCD_W 320
#define LCD_H 240

void LCD_Init(void);
void LCD_Fill(uint16_t color);
void LCD_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void LCD_DrawPixel(int16_t x, int16_t y, uint16_t color);
void LCD_DrawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void LCD_DrawString(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
int16_t LCD_StrWidth(const char *s, uint8_t scale);

/* colors */
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GRAY    0x8410
#define C_ORANGE  0xFD20

#endif