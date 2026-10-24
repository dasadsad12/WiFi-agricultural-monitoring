#include "stm32f10x.h"
#include "OLED.h"
#include "delay.h"

static u8 oled_buffer[8][128];

static const u8 oled_digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};

static const u8 oled_upper[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};

/* Standard 8x16 ASCII glyphs, matching the font used by the verified
   wearable-monitor project.  Only characters used by live sensor values
   are stored here to keep the STM32F103 firmware compact. */
static const u8 oled_digits16[10][16] = {
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x0F,0x10,0x20,0x20,0x10,0x0F,0x00},
    {0x00,0x10,0x10,0xF8,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00},
    {0x00,0x70,0x08,0x08,0x08,0x88,0x70,0x00,0x00,0x30,0x28,0x24,0x22,0x21,0x30,0x00},
    {0x00,0x30,0x08,0x88,0x88,0x48,0x30,0x00,0x00,0x18,0x20,0x20,0x20,0x11,0x0E,0x00},
    {0x00,0x00,0xC0,0x20,0x10,0xF8,0x00,0x00,0x00,0x07,0x04,0x24,0x24,0x3F,0x24,0x00},
    {0x00,0xF8,0x08,0x88,0x88,0x08,0x08,0x00,0x00,0x19,0x21,0x20,0x20,0x11,0x0E,0x00},
    {0x00,0xE0,0x10,0x88,0x88,0x18,0x00,0x00,0x00,0x0F,0x11,0x20,0x20,0x11,0x0E,0x00},
    {0x00,0x38,0x08,0x08,0xC8,0x38,0x08,0x00,0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x70,0x88,0x08,0x08,0x88,0x70,0x00,0x00,0x1C,0x22,0x21,0x21,0x22,0x1C,0x00},
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x00,0x31,0x22,0x22,0x11,0x0F,0x00}
};

static const u8 oled_percent16[16] = {
    0xF0,0x08,0xF0,0x00,0xE0,0x18,0x00,0x00,0x00,0x21,0x1C,0x03,0x1E,0x21,0x1E,0x00
};
static const u8 oled_comma16[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0xB0,0x70,0x00,0x00,0x00,0x00,0x00
};
static const u8 oled_minus16[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01
};
static const u8 oled_dot16[16] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00,0x00,0x00
};
static const u8 oled_colon16[16] = {
    0x00,0x00,0x00,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00
};
static const u8 oled_a16[16] = {
    0x00,0x00,0xC0,0x38,0xE0,0x00,0x00,0x00,0x20,0x3C,0x23,0x02,0x02,0x27,0x38,0x20
};
static const u8 oled_c16[16] = {
    0xC0,0x30,0x08,0x08,0x08,0x08,0x38,0x00,0x07,0x18,0x20,0x20,0x20,0x10,0x08,0x00
};
static const u8 oled_g16[16] = {
    0xC0,0x30,0x08,0x08,0x08,0x38,0x00,0x00,0x07,0x18,0x20,0x20,0x22,0x1E,0x02,0x00
};
static const u8 oled_i16[16] = {
    0x00,0x08,0x08,0xF8,0x08,0x08,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00
};
static const u8 oled_p16[16] = {
    0x08,0xF8,0x08,0x08,0x08,0x08,0xF0,0x00,0x20,0x3F,0x21,0x01,0x01,0x01,0x00,0x00
};
static const u8 oled_s16[16] = {
    0x00,0x70,0x88,0x08,0x08,0x08,0x38,0x00,0x00,0x38,0x20,0x21,0x21,0x22,0x1C,0x00
};
static const u8 oled_t16[16] = {
    0x18,0x08,0x08,0xF8,0x08,0x08,0x18,0x00,0x00,0x00,0x20,0x3F,0x20,0x00,0x00,0x00
};
static const u8 oled_w16[16] = {
    0xF8,0x08,0x00,0xF8,0x00,0x08,0xF8,0x00,0x03,0x3C,0x07,0x00,0x07,0x3C,0x03,0x00
};
static const u8 oled_greater16[16] = {
    0x00,0x04,0x08,0x10,0x20,0x40,0x80,0x00,
    0x00,0x10,0x08,0x04,0x02,0x01,0x00,0x00
};

static void OLED_SCL_High(void) { GPIO_SetBits(OLED_PORT, OLED_SCL_PIN); }
static void OLED_SCL_Low(void)  { GPIO_ResetBits(OLED_PORT, OLED_SCL_PIN); }
static void OLED_SDA_High(void) { GPIO_SetBits(OLED_PORT, OLED_SDA_PIN); }
static void OLED_SDA_Low(void)  { GPIO_ResetBits(OLED_PORT, OLED_SDA_PIN); }

static void OLED_I2C_Delay(void)
{
    delay_us(1);
}

static void OLED_I2C_Start(void)
{
    OLED_SDA_High();
    OLED_SCL_High();
    OLED_I2C_Delay();
    OLED_SDA_Low();
    OLED_I2C_Delay();
    OLED_SCL_Low();
}

static void OLED_I2C_Stop(void)
{
    OLED_SDA_Low();
    OLED_SCL_High();
    OLED_I2C_Delay();
    OLED_SDA_High();
}

static void OLED_I2C_WriteByte(u8 data)
{
    u8 bit;

    for (bit = 0; bit < 8; bit++)
    {
        if (data & 0x80) OLED_SDA_High();
        else OLED_SDA_Low();
        OLED_SCL_High();
        OLED_I2C_Delay();
        OLED_SCL_Low();
        data <<= 1;
    }
    OLED_SDA_High();
    OLED_SCL_High();
    OLED_I2C_Delay();
    OLED_SCL_Low();
}

static void OLED_WriteCommand(u8 command)
{
    OLED_I2C_Start();
    OLED_I2C_WriteByte(OLED_I2C_ADDR);
    OLED_I2C_WriteByte(0x00);
    OLED_I2C_WriteByte(command);
    OLED_I2C_Stop();
}

static void OLED_SetPosition(u8 x, u8 page)
{
    OLED_WriteCommand((u8)(0xB0 | page));
    OLED_WriteCommand((u8)(x & 0x0F));
    OLED_WriteCommand((u8)(0x10 | ((x >> 4) & 0x0F)));
}

static const u8 *OLED_GetGlyph(char character)
{
    static const u8 blank[5] = {0,0,0,0,0};
    static const u8 colon[5] = {0,0x36,0x36,0,0};
    static const u8 comma[5] = {0,0x50,0x30,0,0};
    static const u8 hyphen[5] = {0x08,0x08,0x08,0x08,0x08};
    static const u8 dot[5] = {0,0x60,0x60,0,0};

    if (character >= 'a' && character <= 'z') character -= ('a' - 'A');
    if (character >= 'A' && character <= 'Z') return oled_upper[character - 'A'];
    if (character >= '0' && character <= '9') return oled_digits[character - '0'];
    if (character == ':') return colon;
    if (character == ',') return comma;
    if (character == '-') return hyphen;
    if (character == '.') return dot;
    return blank;
}

static const u8 *OLED_GetGlyph16(char character)
{
    static const u8 blank[16] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    if (character >= 'a' && character <= 'z') character -= ('a' - 'A');
    if (character >= '0' && character <= '9')
        return oled_digits16[character - '0'];
    if (character == '%') return oled_percent16;
    if (character == ',') return oled_comma16;
    if (character == '-') return oled_minus16;
    if (character == '.') return oled_dot16;
    if (character == ':') return oled_colon16;
    if (character == 'A') return oled_a16;
    if (character == 'C') return oled_c16;
    if (character == 'G') return oled_g16;
    if (character == 'I') return oled_i16;
    if (character == 'P') return oled_p16;
    if (character == 'S') return oled_s16;
    if (character == 'T') return oled_t16;
    if (character == 'W') return oled_w16;
    if (character == '>') return oled_greater16;
    return blank;
}

void OLED_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    gpio.GPIO_Pin = OLED_SCL_PIN | OLED_SDA_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(OLED_PORT, &gpio);
    OLED_SCL_High();
    OLED_SDA_High();
    delay_ms(20);

    OLED_WriteCommand(0xAE);
    OLED_WriteCommand(0xD5); OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xA8); OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xD3); OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0x8D); OLED_WriteCommand(0x14);
    OLED_WriteCommand(0x20); OLED_WriteCommand(0x02);
    OLED_WriteCommand(0xA1); OLED_WriteCommand(0xC8);
    OLED_WriteCommand(0xDA); OLED_WriteCommand(0x12);
    OLED_WriteCommand(0x81); OLED_WriteCommand(0xCF);
    OLED_WriteCommand(0xD9); OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDB); OLED_WriteCommand(0x40);
    OLED_WriteCommand(0xA4); OLED_WriteCommand(0xA6);
    OLED_WriteCommand(0xAF);
    OLED_Clear();
    OLED_Update();
}

void OLED_Clear(void)
{
    u8 page;
    u8 column;

    for (page = 0; page < 8; page++)
        for (column = 0; column < 128; column++) oled_buffer[page][column] = 0;
}

void OLED_Update(void)
{
    u8 page;
    u8 column;

    for (page = 0; page < 8; page++)
    {
        OLED_SetPosition(0, page);
        OLED_I2C_Start();
        OLED_I2C_WriteByte(OLED_I2C_ADDR);
        OLED_I2C_WriteByte(0x40);
        for (column = 0; column < 128; column++) OLED_I2C_WriteByte(oled_buffer[page][column]);
        OLED_I2C_Stop();
    }
}

void OLED_ShowChar(u8 x, u8 page, char character)
{
    const u8 *glyph;
    u8 column;

    if (x > 122 || page > 7) return;
    glyph = OLED_GetGlyph(character);
    for (column = 0; column < 5; column++) oled_buffer[page][x + column] = glyph[column];
    oled_buffer[page][x + 5] = 0;
}

void OLED_ShowString(u8 x, u8 page, const char *string)
{
    while (*string != '\0' && x <= 122)
    {
        OLED_ShowChar(x, page, *string);
        x += 6;
        string++;
    }
}

void OLED_ShowChinese16(u8 x, u8 page, const u8 *bitmap)
{
    u8 row;
    u8 column;
    u8 mask;

    if (x > 112 || page > 6 || bitmap == 0) return;

    for (row = 0; row < 16; row++)
    {
        for (column = 0; column < 16; column++)
        {
            mask = (u8)(0x80 >> (column & 7));
            if (bitmap[row * 2 + (column >> 3)] & mask)
                oled_buffer[page + (row >> 3)][x + column] |=
                    (u8)(1U << (row & 7));
            else
                oled_buffer[page + (row >> 3)][x + column] &=
                    (u8)~(1U << (row & 7));
        }
    }
}

void OLED_ShowChinese8(u8 x, u8 page, const u8 *bitmap)
{
    u8 row;
    u8 column;
    u8 source_row;
    u8 source_column;
    u8 source_x;
    u8 source_y;
    u8 mask;
    u8 pixel;

    if (x > 120 || page > 7 || bitmap == 0) return;

    for (row = 0; row < 8; row++)
    {
        for (column = 0; column < 8; column++)
        {
            pixel = 0;
            for (source_row = 0; source_row < 2; source_row++)
            {
                for (source_column = 0; source_column < 2; source_column++)
                {
                    source_x = (u8)(column * 2 + source_column);
                    source_y = (u8)(row * 2 + source_row);
                    mask = (u8)(0x80 >> (source_x & 7));
                    if (bitmap[source_y * 2 + (source_x >> 3)] & mask)
                        pixel = 1;
                }
            }
            if (pixel)
                oled_buffer[page][x + column] |= (u8)(1U << row);
            else
                oled_buffer[page][x + column] &= (u8)~(1U << row);
        }
    }
}

void OLED_ShowChinese12(u8 x, u8 y, const u8 *bitmap)
{
    u8 row;
    u8 column;
    u8 source_x;
    u8 source_y;
    u8 source_x_end;
    u8 source_y_end;
    u8 test_x;
    u8 test_y;
    u8 mask;
    u8 pixel;
    u8 absolute_y;
    u8 page;
    u8 bit;

    if (x > 116 || y > 52 || bitmap == 0) return;

    /* Resample the existing 16x16 glyph to 12x12.  Pixel-coordinate
       rendering lets five rows fit in the 64-pixel OLED height. */
    for (row = 0; row < 12; row++)
    {
        source_y = (u8)(((u16)row * 16) / 12);
        for (column = 0; column < 12; column++)
        {
            source_x = (u8)(((u16)column * 16) / 12);
            source_x_end = (u8)(((u16)(column + 1) * 16) / 12);
            source_y_end = (u8)(((u16)(row + 1) * 16) / 12);
            mask = (u8)(0x80 >> (source_x & 7));
            pixel = 0;
            for (test_y = source_y; test_y < source_y_end; test_y++)
            {
                for (test_x = source_x; test_x < source_x_end; test_x++)
                {
                    mask = (u8)(0x80 >> (test_x & 7));
                    if (bitmap[test_y * 2 + (test_x >> 3)] & mask)
                        pixel = 1;
                }
            }
            absolute_y = (u8)(y + row);
            page = (u8)(absolute_y >> 3);
            bit = (u8)(1U << (absolute_y & 7));
            if (pixel) oled_buffer[page][x + column] |= bit;
            else oled_buffer[page][x + column] &= (u8)~bit;
        }
    }
}

void OLED_ShowChinese14(u8 x, u8 y, const u8 *bitmap)
{
    u8 row;
    u8 column;
    u8 source_x;
    u8 source_y;
    u8 source_x_end;
    u8 source_y_end;
    u8 test_x;
    u8 test_y;
    u8 mask;
    u8 pixel;
    u8 absolute_y;
    u8 page;
    u8 bit;

    if (x > 114 || y > 50 || bitmap == 0) return;

    /* Keep the original 16x16 font but reduce it only slightly for a
       readable 14x14 glyph that still leaves room for four OLED rows. */
    for (row = 0; row < 14; row++)
    {
        source_y = (u8)(((u16)row * 16) / 14);
        source_y_end = (u8)(((u16)(row + 1) * 16) / 14);
        for (column = 0; column < 14; column++)
        {
            source_x = (u8)(((u16)column * 16) / 14);
            source_x_end = (u8)(((u16)(column + 1) * 16) / 14);
            pixel = 0;
            for (test_y = source_y; test_y < source_y_end; test_y++)
            {
                for (test_x = source_x; test_x < source_x_end; test_x++)
                {
                    mask = (u8)(0x80 >> (test_x & 7));
                    if (bitmap[test_y * 2 + (test_x >> 3)] & mask)
                        pixel = 1;
                }
            }
            absolute_y = (u8)(y + row);
            page = (u8)(absolute_y >> 3);
            bit = (u8)(1U << (absolute_y & 7));
            if (pixel) oled_buffer[page][x + column] |= bit;
            else oled_buffer[page][x + column] &= (u8)~bit;
        }
    }
}

void OLED_ShowColon12(u8 x, u8 y)
{
    u8 row;
    u8 column;
    u8 absolute_y;
    u8 page;
    u8 bit;
    u8 pixel;

    if (x > 116 || y > 52) return;
    for (row = 0; row < 12; row++)
    {
        for (column = 0; column < 12; column++)
        {
            pixel = (u8)(((column >= 4 && column <= 7) &&
                          (row >= 2 && row <= 3 || row >= 8 && row <= 9)));
            absolute_y = (u8)(y + row);
            page = (u8)(absolute_y >> 3);
            bit = (u8)(1U << (absolute_y & 7));
            if (pixel) oled_buffer[page][x + column] |= bit;
            else oled_buffer[page][x + column] &= (u8)~bit;
        }
    }
}

void OLED_ShowColon14(u8 x, u8 y)
{
    u8 row;
    u8 column;
    u8 absolute_y;
    u8 page;
    u8 bit;
    u8 pixel;

    if (x > 114 || y > 50) return;
    for (row = 0; row < 14; row++)
    {
        for (column = 0; column < 14; column++)
        {
            pixel = (u8)(((column >= 5 && column <= 8) &&
                          (row >= 2 && row <= 4 ||
                           row >= 9 && row <= 11)));
            absolute_y = (u8)(y + row);
            page = (u8)(absolute_y >> 3);
            bit = (u8)(1U << (absolute_y & 7));
            if (pixel) oled_buffer[page][x + column] |= bit;
            else oled_buffer[page][x + column] &= (u8)~bit;
        }
    }
}

void OLED_ShowAscii8(u8 x, u8 y, const char *string)
{
    const u8 *glyph;
    u8 row;
    u8 column;
    u8 absolute_y;
    u8 page;
    u8 bit;
    u8 pixel;

    while (*string != '\0' && x <= 122 && y <= 56)
    {
        glyph = OLED_GetGlyph(*string);
        for (row = 0; row < 8; row++)
        {
            for (column = 0; column < 6; column++)
            {
                /* ASCII glyphs are stored as five vertical columns. */
                pixel = (u8)(column < 5 &&
                             (glyph[column] & (1U << row)));
                absolute_y = (u8)(y + row);
                page = (u8)(absolute_y >> 3);
                bit = (u8)(1U << (absolute_y & 7));
                if (pixel) oled_buffer[page][x + column] |= bit;
                else oled_buffer[page][x + column] &= (u8)~bit;
            }
        }
        x = (u8)(x + 6);
        string++;
    }
}

void OLED_ShowAscii16(u8 x, u8 y, const char *string)
{
    const u8 *glyph;
    u8 page;
    u8 column;

    if (string == 0 || (y & 7U) != 0U || y > 48U) return;
    page = (u8)(y >> 3);
    while (*string != '\0' && x <= 120U)
    {
        glyph = OLED_GetGlyph16(*string);
        for (column = 0; column < 8U; column++)
        {
            oled_buffer[page][x + column] = glyph[column];
            oled_buffer[page + 1U][x + column] = glyph[column + 8U];
        }
        x = (u8)(x + 8U);
        string++;
    }
}
