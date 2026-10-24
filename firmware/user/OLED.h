#ifndef __OLED_H
#define __OLED_H

#include "stm32f10x.h"

#define OLED_PORT       GPIOB
#define OLED_SCL_PIN    GPIO_Pin_6
#define OLED_SDA_PIN    GPIO_Pin_7
#define OLED_I2C_ADDR   0x78

void OLED_Init(void);
void OLED_Clear(void);
void OLED_Update(void);
void OLED_ShowChar(u8 x, u8 page, char character);
void OLED_ShowString(u8 x, u8 page, const char *string);
void OLED_ShowChinese16(u8 x, u8 page, const u8 *bitmap);
void OLED_ShowChinese8(u8 x, u8 page, const u8 *bitmap);
void OLED_ShowChinese12(u8 x, u8 y, const u8 *bitmap);
void OLED_ShowColon12(u8 x, u8 y);
void OLED_ShowChinese14(u8 x, u8 y, const u8 *bitmap);
void OLED_ShowColon14(u8 x, u8 y);
void OLED_ShowAscii8(u8 x, u8 y, const char *string);
void OLED_ShowAscii16(u8 x, u8 y, const char *string);

#endif
