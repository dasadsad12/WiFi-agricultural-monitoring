#include "stm32f10x.h"
#include "delay.h"
#include "OLED.h"
#include "WifiRelay.h"
#include "SlaveFont.h"
#include "MenuFont.h"
#include "AgriFont.h"
#include "stm32f10x_usart.h"
#include "misc.h"
#include <stdio.h>
#include <string.h>

/*
 * LORA智慧农业从机
 *
 * 传感器：
 *   PA1  - MK011169 土壤湿度模拟量
 *   PB0  - 水位传感器 S 模拟量
 *   PB10 - CO2 模块 RX（STM32 TX）
 *   PB11 - CO2 模块 TX（STM32 USART3_RX，按当前直连硬件连接）
 *   PB6/PB7 - SHT30、BH1750 软件 I2C
 * 继电器：
 *   PB3 - RLY1 水泵
 *   PB4 - RLY2 补光灯
 *   PB5 - RLY3 风扇
 *
 * 当前硬件没有独立的土壤温度探头，因此 SHT30 在协议中明确表示为
 * 环境温度/环境湿度，PA1 只表示土壤湿度。
 */

#define ALARM_LED_PORT GPIOA
#define ALARM_LED_PIN  GPIO_Pin_12
#define ALARM_BEEP_PORT GPIOA
#define ALARM_BEEP_PIN  GPIO_Pin_15

#define RELAY_PORT GPIOB
#define RELAY_PUMP_PIN  GPIO_Pin_3
#define RELAY_LIGHT_PIN GPIO_Pin_4
#define RELAY_FAN_PIN   GPIO_Pin_5
#define RELAY_MASK_ALL  (RELAY_PUMP_PIN | RELAY_LIGHT_PIN | RELAY_FAN_PIN)

#define BH1750_PORT            GPIOB
#define BH1750_SCL_PIN         GPIO_Pin_6
#define BH1750_SDA_PIN         GPIO_Pin_7
#define BH1750_ADDR_WRITE      0x46
#define BH1750_ADDR_READ       0x47
#define BH1750_POWER_ON        0x01
#define BH1750_RESET           0x07
#define BH1750_ONE_TIME_L_RES_MODE 0x23

#define SHT30_ADDR_WRITE       0x88
#define SHT30_ADDR_READ        0x89
#define SHT30_MEASURE_MSB      0x24
#define SHT30_MEASURE_LSB      0x00

#define WATER_ADC_CHANNEL      8U
#define SOIL_ADC_CHANNEL       1U
#define SENSOR_ADC_ERROR       65535U
#define SENSOR_LUX_ERROR       65535U
#define SHT30_TEMPERATURE_ERROR (-32768)
#define SHT30_HUMIDITY_ERROR   65535U

/* These calibration pairs are deliberately centralised so they can be
   adjusted after measuring the user's actual probes. */
#define WATER_RAW_EMPTY        250U
#define WATER_RAW_FULL         3800U
#define SOIL_RAW_DRY           3800U
#define SOIL_RAW_WET           1200U

#define ADC_AVERAGE_SAMPLES    4U
#define ADC_CONVERSION_TIMEOUT 100000UL

#define CO2_BAUDRATE           9600U
#define CO2_RX_SIZE            96U
#define CO2_NO_FRAME_TIMEOUT   15000UL
#define ALARM_STARTUP_GUARD_MS 3000UL
#define ALARM_CONFIRM_SAMPLE_COUNT 3U

#ifndef AGRI_SLAVE_ID
#define AGRI_SLAVE_ID          1U
#endif
#define SLAVE_ID               AGRI_SLAVE_ID
#define WIFI_CONFIG_OK         1U
#define SENSOR_REFRESH_MS      1000UL
#define DISPLAY_REFRESH_MS     500UL
#define RADIO_REINIT_MS        60000UL
#define WATCHDOG_RELOAD_VALUE  4095U
#define SLAVE_DISPLAY_PAGE_COUNT 3U

#define KEY_PORT              GPIOA
#define KEY1_PIN              GPIO_Pin_5
#define KEY2_PIN              GPIO_Pin_6
#define KEY3_PIN              GPIO_Pin_7
#define KEY4_PIN              GPIO_Pin_8
#define KEY_ACTIVE_LEVEL      0U
#define KEY_DEBOUNCE_MS       20U

#define LIGHT_ALARM_DARK       0U
#define LIGHT_ALARM_LOW        1U
#define LIGHT_ALARM_HIGH       2U
#define LUX_DARK_MAX           9U
#define LUX_LOW_MAX            199U

typedef struct
{
    u16 sequence;
    u16 water_raw;
    u8 water_pct;
    u8 water_valid;
    u16 soil_raw;
    u8 soil_pct;
    u8 soil_valid;
    u16 co2_ppm;
    u8 co2_valid;
    u16 lux;
    s16 temperature_x10;
    u16 humidity_x10;
    u8 sht30_valid;
    u8 alarm_enabled;
    u8 water_threshold;
    u16 co2_threshold;
    u8 soil_threshold;
    s16 temp_threshold_x10;
    u16 humidity_threshold_x10;
    u8 light_alarm_mode;
    u8 water_alarm;
    u8 co2_alarm;
    u8 soil_alarm;
    u8 temp_alarm;
    u8 humidity_alarm;
    u8 light_alarm;
    u8 pump_state;
    u8 light_state;
    u8 fan_state;
    u8 alarm_test_active;
} AgriculturePacket;

typedef struct
{
    u8 alarm_enabled;
    u8 water_threshold;
    u16 co2_threshold;
    u8 soil_threshold;
    s16 temp_threshold_x10;
    u16 humidity_threshold_x10;
    u8 light_alarm_mode;
} AgricultureSettings;

static AgricultureSettings settings;
static u8 manual_mask;
static u8 manual_state;
static volatile u16 relay_test_ms;
static volatile u16 alarm_test_ms;
static u8 relay_test_step;

static volatile u8 co2_rx_buffer[CO2_RX_SIZE];
static volatile u8 co2_rx_head;
static volatile u8 co2_rx_tail;
static u16 co2_ppm;
static u8 co2_protocol;
static u8 co2_valid;
static u32 co2_last_frame_ms;
static u8 co2_scan_window[9];
static u8 co2_scan_count;
static u8 sensor_i2c_error_streak;

static const u8 ui_comm_label[] = {SLAVE_HZ_TONG, SLAVE_HZ_XUN};
static const u8 ui_connected_text[] = {
    SLAVE_HZ_YI, SLAVE_HZ_LIAN, SLAVE_HZ_JIE
};
static const u8 ui_disconnected_text[] = {
    SLAVE_HZ_WEI, SLAVE_HZ_LIAN, SLAVE_HZ_JIE
};
static const u8 ui_alarm_label[] = {SLAVE_HZ_BAO, SLAVE_HZ_JING};
static const u8 ui_status_label[] = {SLAVE_HZ_ZHUANG, SLAVE_HZ_TAI};
static const u8 ui_settings_title[] = {
    MENU_HZ_XIANG, MENU_HZ_GUAN, MENU_HZ_SHE, MENU_HZ_ZHI
};
static u8 slave_screen_force;
static u8 sensor_alarm_armed;
static u8 alarm_water_streak;
static u8 alarm_co2_streak;
static u8 alarm_soil_streak;
static u8 alarm_temp_streak;
static u8 alarm_humidity_streak;
static u8 alarm_light_streak;
static u16 alarm_last_sequence;
static u8 alarm_sequence_seen;

void Watchdog_Feed(void)
{
    IWDG->KR = 0xAAAAU;
}

static void Watchdog_Init(void)
{
    u32 timeout;
    RCC->CSR |= RCC_CSR_LSION;
    timeout = 60000UL;
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U && timeout != 0U)
        timeout--;
    IWDG->KR = 0x5555U;
    IWDG->PR = 0x06U;
    IWDG->RLR = WATCHDOG_RELOAD_VALUE;
    timeout = 60000UL;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U &&
           timeout != 0U)
        timeout--;
    Watchdog_Feed();
    IWDG->KR = 0xCCCCU;
}

static void Safe_DelayMs(u16 value)
{
    while (value != 0U)
    {
        delay_ms(1);
        Watchdog_Feed();
        value--;
    }
}

void CO2_RxByte(u8 byte)
{
    u8 next;
    next = (u8)(co2_rx_head + 1U);
    if (next >= CO2_RX_SIZE) next = 0U;
    if (next != co2_rx_tail)
    {
        co2_rx_buffer[co2_rx_head] = byte;
        co2_rx_head = next;
    }
}

static u8 CO2_ReadByte(u8 *byte)
{
    if (co2_rx_tail == co2_rx_head) return 0U;
    *byte = co2_rx_buffer[co2_rx_tail];
    co2_rx_tail++;
    if (co2_rx_tail >= CO2_RX_SIZE) co2_rx_tail = 0U;
    return 1U;
}

static void CO2_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef nvic;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_11;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate = CO2_BAUDRATE;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART3, &usart);
    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);
    nvic.NVIC_IRQChannel = USART3_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 2;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    USART_Cmd(USART3, ENABLE);
    co2_rx_head = 0U;
    co2_rx_tail = 0U;
    co2_ppm = 0U;
    co2_protocol = 0U;
    co2_valid = 0U;
    co2_last_frame_ms = 0UL;
    co2_scan_count = 0U;
}

static u8 CO2_ChecksumShortAdd(const u8 *data)
{
    u8 index;
    u8 sum;
    sum = 0U;
    for (index = 0U; index < 5U; index++)
        sum = (u8)(sum + data[index]);
    return sum;
}

static u8 CO2_ChecksumShortTwos(const u8 *data)
{
    u8 index;
    u8 sum;
    sum = 0U;
    for (index = 1U; index < 5U; index++)
        sum = (u8)(sum + data[index]);
    return (u8)(0xFFU - sum + 1U);
}

static u8 CO2_ChecksumLongTwos(const u8 *data)
{
    u8 index;
    u8 sum;
    sum = 0U;
    for (index = 1U; index < 8U; index++)
        sum = (u8)(sum + data[index]);
    return (u8)(0xFFU - sum + 1U);
}

static void CO2_AcceptFrame(u16 ppm, u8 protocol, u32 now_ms)
{
    /*
     * The cloud contract caps CO₂ at 50000 ppm.  Noise, a wrong baud rate,
     * or a different module frame must not turn one corrupt 16-bit value into
     * a rejected telemetry packet; keep the previous value and let the
     * timeout logic mark the sensor invalid instead.
     */
    if (ppm > 50000U) return;
    co2_ppm = ppm;
    co2_protocol = protocol;
    co2_valid = 1U;
    co2_last_frame_ms = now_ms;
}

static void CO2_ParseByte(u8 byte, u32 now_ms)
{
    u8 index;
    u8 *short_frame;
    u8 *long_frame;

    if (co2_scan_count < sizeof(co2_scan_window))
    {
        co2_scan_window[co2_scan_count++] = byte;
    }
    else
    {
        for (index = 0U; index < sizeof(co2_scan_window) - 1U; index++)
            co2_scan_window[index] = co2_scan_window[index + 1U];
        co2_scan_window[sizeof(co2_scan_window) - 1U] = byte;
    }

    if (co2_scan_count >= 6U)
    {
        short_frame = &co2_scan_window[co2_scan_count - 6U];
        if (short_frame[0] == 0x2CU &&
            CO2_ChecksumShortAdd(short_frame) == short_frame[5])
        {
            CO2_AcceptFrame((u16)(((u16)short_frame[1] << 8) |
                                  short_frame[2]), 1U, now_ms);
            co2_scan_count = 0U;
            return;
        }

        if (short_frame[0] == 0xFFU && short_frame[1] == 0x86U &&
            (short_frame[5] == CO2_ChecksumShortAdd(short_frame) ||
             short_frame[5] == CO2_ChecksumShortTwos(short_frame)))
        {
            CO2_AcceptFrame((u16)(((u16)short_frame[3] << 8) |
                                  short_frame[4]), 2U, now_ms);
            co2_scan_count = 0U;
            return;
        }
    }

    if (co2_scan_count >= sizeof(co2_scan_window))
    {
        long_frame = co2_scan_window;
        if (long_frame[0] == 0xFFU && long_frame[1] == 0x86U &&
            CO2_ChecksumLongTwos(long_frame) == long_frame[8])
        {
            CO2_AcceptFrame((u16)(((u16)long_frame[2] << 8) |
                                  long_frame[3]), 3U, now_ms);
            co2_scan_count = 0U;
        }
    }
    (void)co2_protocol;
}

static void CO2_Service(u32 now_ms)
{
    u8 byte;
    u8 count;

    /*
     * JW01 CO2 module variants commonly output either:
     *   2C CO2_H CO2_L RANGE_H RANGE_L SUM
     * or:
     *   FF 86 CO2_H CO2_L ... CHECKSUM
     * Receive only; no query command is required for the user's JW01 module.
     */
    while (USART_GetFlagStatus(USART3, USART_FLAG_RXNE) != RESET)
        CO2_RxByte((u8)USART_ReceiveData(USART3));
    if ((USART3->SR & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE)) != 0U)
    {
        (void)USART3->SR;
        (void)USART3->DR;
    }

    count = 0U;
    while (count < 48U && CO2_ReadByte(&byte))
    {
        count++;
        CO2_ParseByte(byte, now_ms);
    }
    if ((u32)(now_ms - co2_last_frame_ms) > CO2_NO_FRAME_TIMEOUT)
        co2_valid = 0U;
}

static void I2C_SCL_High(void) { GPIO_SetBits(BH1750_PORT, BH1750_SCL_PIN); }
static void I2C_SCL_Low(void)  { GPIO_ResetBits(BH1750_PORT, BH1750_SCL_PIN); }
static void I2C_SDA_High(void) { GPIO_SetBits(BH1750_PORT, BH1750_SDA_PIN); }
static void I2C_SDA_Low(void)  { GPIO_ResetBits(BH1750_PORT, BH1750_SDA_PIN); }
static void I2C_Delay(void) { delay_us(2U); }

static u8 I2C_SCL_Read(void)
{
    return (u8)(GPIO_ReadInputDataBit(BH1750_PORT, BH1750_SCL_PIN) != 0);
}

static u8 I2C_SDA_Read(void)
{
    return (u8)(GPIO_ReadInputDataBit(BH1750_PORT, BH1750_SDA_PIN) != 0);
}

static u8 I2C_SCL_Ready(void)
{
    u8 retry;
    I2C_SCL_High();
    for (retry = 0U; retry < 20U; retry++)
    {
        I2C_Delay();
        if (I2C_SCL_Read()) return 1U;
    }
    return 0U;
}

static void I2C_Start(void)
{
    I2C_SDA_High();
    I2C_SCL_Ready();
    I2C_Delay();
    I2C_SDA_Low();
    I2C_Delay();
    I2C_SCL_Low();
}

static void I2C_Stop(void)
{
    I2C_SDA_Low();
    I2C_SCL_Ready();
    I2C_Delay();
    I2C_SDA_High();
    I2C_Delay();
}

/*
 * Release a sensor bus that was left in the middle of a transaction during
 * power-up or an ESP8266 reset.  The agriculture board shares PB6/PB7
 * between BH1750 and SHT30, so one stuck-low device must not make both
 * sensors appear permanently invalid.
 */
static void I2C_BusRecover(void)
{
    u8 index;
    I2C_SDA_High();
    for (index = 0U; index < 9U; index++)
    {
        I2C_SCL_Low();
        I2C_Delay();
        I2C_SCL_High();
        I2C_Delay();
    }
    I2C_Stop();
}

static u8 I2C_WriteByte(u8 data)
{
    u8 index;
    u8 ack;
    for (index = 0U; index < 8U; index++)
    {
        if (data & 0x80U) I2C_SDA_High();
        else I2C_SDA_Low();
        data <<= 1;
        I2C_Delay();
        if (!I2C_SCL_Ready()) return 0U;
        I2C_Delay();
        I2C_SCL_Low();
    }
    I2C_SDA_High();
    I2C_Delay();
    if (!I2C_SCL_Ready()) return 0U;
    I2C_Delay();
    ack = (u8)!I2C_SDA_Read();
    I2C_SCL_Low();
    return ack;
}

static u8 I2C_ReadByte(u8 ack, u8 *value)
{
    u8 index;
    u8 data;
    data = 0U;
    I2C_SDA_High();
    for (index = 0U; index < 8U; index++)
    {
        data <<= 1;
        if (!I2C_SCL_Ready()) return 0U;
        I2C_Delay();
        if (I2C_SDA_Read()) data |= 1U;
        I2C_SCL_Low();
        I2C_Delay();
    }
    if (ack) I2C_SDA_Low();
    else I2C_SDA_High();
    I2C_Delay();
    if (!I2C_SCL_Ready()) return 0U;
    I2C_Delay();
    I2C_SCL_Low();
    I2C_SDA_High();
    *value = data;
    return 1U;
}

static u8 I2C_WriteCommand(u8 command)
{
    u8 ok;
    I2C_Start();
    ok = I2C_WriteByte(BH1750_ADDR_WRITE);
    if (ok) ok = I2C_WriteByte(command);
    I2C_Stop();
    return ok;
}

static u8 SHT30_Crc8(const u8 *data)
{
    u8 crc;
    u8 index;
    u8 bit;
    crc = 0xFFU;
    for (index = 0U; index < 2U; index++)
    {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; bit++)
            crc = (u8)((crc & 0x80U) ? ((crc << 1) ^ 0x31U) :
                       (crc << 1));
    }
    return crc;
}

static u8 SHT30_Read(s16 *temperature_x10, u16 *humidity_x10)
{
    u8 data[6];
    u8 index;
    u16 raw_temperature;
    u16 raw_humidity;

    I2C_Start();
    if (!I2C_WriteByte(SHT30_ADDR_WRITE))
    {
        I2C_Stop();
        return 0U;
    }
    if (!I2C_WriteByte(SHT30_MEASURE_MSB) ||
        !I2C_WriteByte(SHT30_MEASURE_LSB))
    {
        I2C_Stop();
        return 0U;
    }
    I2C_Stop();
    Safe_DelayMs(20U);
    I2C_Start();
    if (!I2C_WriteByte(SHT30_ADDR_READ))
    {
        I2C_Stop();
        return 0U;
    }
    for (index = 0U; index < 6U; index++)
    {
        if (!I2C_ReadByte(index < 5U, &data[index]))
        {
            I2C_Stop();
            return 0U;
        }
    }
    I2C_Stop();
    if (SHT30_Crc8(&data[0]) != data[2] ||
        SHT30_Crc8(&data[3]) != data[5])
        return 0U;
    raw_temperature = (u16)(((u16)data[0] << 8) | data[1]);
    raw_humidity = (u16)(((u16)data[3] << 8) | data[4]);
    *temperature_x10 = (s16)(-450 + (s32)1750 * raw_temperature / 65535);
    *humidity_x10 = (u16)((u32)1000 * raw_humidity / 65535);
    if (*temperature_x10 < -400 || *temperature_x10 > 1250 ||
        *humidity_x10 > 1000U)
        return 0U;
    return 1U;
}

static u16 BH1750_ReadLux(void)
{
    u8 high_byte;
    u8 low_byte;
    u16 raw;
    I2C_WriteCommand(BH1750_ONE_TIME_L_RES_MODE);
    Safe_DelayMs(24U);
    I2C_Start();
    if (!I2C_WriteByte(BH1750_ADDR_READ))
    {
        I2C_Stop();
        return SENSOR_LUX_ERROR;
    }
    if (!I2C_ReadByte(1U, &high_byte) ||
        !I2C_ReadByte(0U, &low_byte))
    {
        I2C_Stop();
        return SENSOR_LUX_ERROR;
    }
    I2C_Stop();
    raw = (u16)(((u16)high_byte << 8) | low_byte);
    return (u16)((u32)raw * 5U / 6U);
}

static void Sensor_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
                           RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_ADC1 |
                           RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_1;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_0;
    GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = BH1750_SCL_PIN | BH1750_SDA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(GPIOB, &gpio);
    I2C_SCL_High();
    I2C_SDA_High();
}

static void Sensor_ADC_Init(void)
{
    u32 timeout;
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);
    /*
     * Reset every regular-conversion control bit before configuring the
     * channels.  The agriculture firmware is also built from a project
     * template that used ADC1 for another sensor set; leaving CR1/CR2
     * trigger bits or a previous sequence behind can make the first channel
     * conversion repeat a stale/full-scale result.
     */
    ADC1->CR1 = 0U;
    ADC1->CR2 = 0U;
    ADC1->SQR1 = 0U;
    ADC1->SQR2 = 0U;
    ADC1->SQR3 = 0U;
    /*
     * PA1 is ADC channel 1 (bits 3..5), while PB0 is ADC channel 8
     * (bits 24..26).  The previous channel-0 setting left PA1 at the
     * default sample time and made the high-impedance soil probe read
     * intermittently.  Keep both agricultural analogue inputs at the
     * longest sample time.
     */
    ADC1->SMPR2 = (7U << 3) | (7U << 24);
    ADC1->CR2 |= ADC_CR2_ADON;
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    timeout = 100000UL;
    while ((ADC1->CR2 & ADC_CR2_RSTCAL) != 0U && timeout != 0U)
        timeout--;
    ADC1->CR2 |= ADC_CR2_CAL;
    timeout = 100000UL;
    while ((ADC1->CR2 & ADC_CR2_CAL) != 0U && timeout != 0U)
        timeout--;
    ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL;
}

static u16 Sensor_ADC_ReadOnce(u8 channel)
{
    u32 timeout;

    ADC1->SQR3 = (u32)(channel & 0x1FU);
    ADC1->SR = 0U;
    ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL | ADC_CR2_SWSTART;
    timeout = ADC_CONVERSION_TIMEOUT;
    while ((ADC1->SR & ADC_SR_EOC) == 0U && timeout != 0U)
        timeout--;
    if (timeout == 0U) return SENSOR_ADC_ERROR;
    return (u16)ADC1->DR;
}

static u16 Sensor_ADC_Read(u8 channel)
{
    u32 sum;
    u16 sample;
    u8 index;

    /*
     * PA1 and PB0 are high-impedance analogue sources.  When SQR3 changes
     * from one channel to the other, the first conversion still contains
     * charge from the previous channel.  Discard it, then average several
     * conversions just like the working WiFi water-quality firmware.
     */
    sample = Sensor_ADC_ReadOnce(channel);
    if (sample == SENSOR_ADC_ERROR) return SENSOR_ADC_ERROR;

    sum = 0UL;
    for (index = 0U; index < ADC_AVERAGE_SAMPLES; index++)
    {
        sample = Sensor_ADC_ReadOnce(channel);
        if (sample == SENSOR_ADC_ERROR) return SENSOR_ADC_ERROR;
        sum += (u32)sample;
    }
    return (u16)(sum / ADC_AVERAGE_SAMPLES);
}

static u8 Raw_To_Pct(u16 raw, u16 low, u16 high, u8 reverse)
{
    u32 value;
    if (raw == SENSOR_ADC_ERROR || high <= low) return 0U;
    if (raw <= low) return reverse ? 100U : 0U;
    if (raw >= high) return reverse ? 0U : 100U;
    value = (u32)(raw - low) * 100U / (u32)(high - low);
    return (u8)(reverse ? (100U - value) : value);
}

static void Sensor_Init(void)
{
    Sensor_GPIO_Init();
    Sensor_ADC_Init();
    Safe_DelayMs(20U);
    I2C_BusRecover();
    I2C_WriteCommand(BH1750_POWER_ON);
    I2C_WriteCommand(BH1750_RESET);
    sensor_i2c_error_streak = 0U;
}

static u8 Lux_State(u16 lux)
{
    if (lux == SENSOR_LUX_ERROR) return 3U;
    if (lux <= LUX_DARK_MAX) return 0U;
    if (lux <= LUX_LOW_MAX) return 1U;
    return 2U;
}

static void Settings_Reset(void)
{
    settings.alarm_enabled = 1U;
    settings.water_threshold = 80U;
    settings.co2_threshold = 1200U;
    settings.soil_threshold = 30U;
    settings.temp_threshold_x10 = 350;
    settings.humidity_threshold_x10 = 800U;
    settings.light_alarm_mode = LIGHT_ALARM_DARK;
}

static void Alarm_ResetStreaks(void)
{
    alarm_water_streak = 0U;
    alarm_co2_streak = 0U;
    alarm_soil_streak = 0U;
    alarm_temp_streak = 0U;
    alarm_humidity_streak = 0U;
    alarm_light_streak = 0U;
}

static void Alarm_UpdateStreak(u8 condition, u8 *streak)
{
    if (!condition)
    {
        *streak = 0U;
    }
    else if (*streak < ALARM_CONFIRM_SAMPLE_COUNT)
    {
        (*streak)++;
    }
}

static void Settings_Clamp(void)
{
    if (settings.water_threshold > 100U) settings.water_threshold = 100U;
    if (settings.co2_threshold > 50000U) settings.co2_threshold = 50000U;
    if (settings.soil_threshold > 100U) settings.soil_threshold = 100U;
    if (settings.temp_threshold_x10 < -400)
        settings.temp_threshold_x10 = -400;
    if (settings.temp_threshold_x10 > 1250)
        settings.temp_threshold_x10 = 1250;
    if (settings.humidity_threshold_x10 > 1000U)
        settings.humidity_threshold_x10 = 1000U;
    if (settings.light_alarm_mode > LIGHT_ALARM_HIGH)
        settings.light_alarm_mode = LIGHT_ALARM_DARK;
    settings.alarm_enabled = settings.alarm_enabled ? 1U : 0U;
}

static void Packet_SyncSettings(AgriculturePacket *packet)
{
    packet->alarm_enabled = settings.alarm_enabled;
    packet->water_threshold = settings.water_threshold;
    packet->co2_threshold = settings.co2_threshold;
    packet->soil_threshold = settings.soil_threshold;
    packet->temp_threshold_x10 = settings.temp_threshold_x10;
    packet->humidity_threshold_x10 = settings.humidity_threshold_x10;
    packet->light_alarm_mode = settings.light_alarm_mode;
}

static void Sensor_UpdateAlarms(AgriculturePacket *packet)
{
    u8 light_state;
    u8 new_sample;

    light_state = Lux_State(packet->lux);
    new_sample = (u8)(!alarm_sequence_seen ||
                      alarm_last_sequence != packet->sequence);
    if (new_sample)
    {
        alarm_sequence_seen = 1U;
        alarm_last_sequence = packet->sequence;
        Alarm_UpdateStreak((u8)(packet->water_valid &&
            packet->water_pct >= settings.water_threshold),
            &alarm_water_streak);
        Alarm_UpdateStreak((u8)(packet->co2_valid &&
            packet->co2_ppm >= settings.co2_threshold),
            &alarm_co2_streak);
        Alarm_UpdateStreak((u8)(packet->soil_valid &&
            packet->soil_pct <= settings.soil_threshold),
            &alarm_soil_streak);
        Alarm_UpdateStreak((u8)(packet->sht30_valid &&
            packet->temperature_x10 >= settings.temp_threshold_x10),
            &alarm_temp_streak);
        Alarm_UpdateStreak((u8)(packet->sht30_valid &&
            packet->humidity_x10 >= settings.humidity_threshold_x10),
            &alarm_humidity_streak);
        Alarm_UpdateStreak((u8)(light_state <= LIGHT_ALARM_HIGH &&
            settings.light_alarm_mode == light_state), &alarm_light_streak);
    }
    packet->water_alarm = (u8)(sensor_alarm_armed &&
        alarm_water_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
    packet->co2_alarm = (u8)(sensor_alarm_armed &&
        alarm_co2_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
    packet->soil_alarm = (u8)(sensor_alarm_armed &&
        alarm_soil_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
    packet->temp_alarm = (u8)(sensor_alarm_armed &&
        alarm_temp_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
    packet->humidity_alarm = (u8)(sensor_alarm_armed &&
        alarm_humidity_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
    packet->light_alarm = (u8)(sensor_alarm_armed &&
        alarm_light_streak >= ALARM_CONFIRM_SAMPLE_COUNT);
}

static void Relay_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    GPIO_ResetBits(RELAY_PORT, RELAY_MASK_ALL);
    gpio.GPIO_Pin = RELAY_MASK_ALL;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(RELAY_PORT, &gpio);
    GPIO_ResetBits(RELAY_PORT, RELAY_MASK_ALL);
}

static void Relay_Apply(AgriculturePacket *packet)
{
    u8 pump;
    u8 light;
    u8 fan;
    pump = packet->water_alarm;
    light = packet->light_alarm;
    fan = packet->co2_alarm;
    if (manual_mask & 0x01U) pump = (u8)((manual_state & 0x01U) != 0U);
    if (manual_mask & 0x02U) light = (u8)((manual_state & 0x02U) != 0U);
    if (manual_mask & 0x04U) fan = (u8)((manual_state & 0x04U) != 0U);
    GPIO_ResetBits(RELAY_PORT, RELAY_MASK_ALL);
    if (pump) GPIO_SetBits(RELAY_PORT, RELAY_PUMP_PIN);
    if (light) GPIO_SetBits(RELAY_PORT, RELAY_LIGHT_PIN);
    if (fan) GPIO_SetBits(RELAY_PORT, RELAY_FAN_PIN);
    packet->pump_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_PUMP_PIN);
    packet->light_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_LIGHT_PIN);
    packet->fan_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_FAN_PIN);
}

static void Relay_ApplyTest(AgriculturePacket *packet)
{
    GPIO_ResetBits(RELAY_PORT, RELAY_MASK_ALL);
    if (relay_test_step == 0U) GPIO_SetBits(RELAY_PORT, RELAY_PUMP_PIN);
    else if (relay_test_step == 1U) GPIO_SetBits(RELAY_PORT, RELAY_LIGHT_PIN);
    else GPIO_SetBits(RELAY_PORT, RELAY_FAN_PIN);
    packet->pump_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_PUMP_PIN);
    packet->light_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_LIGHT_PIN);
    packet->fan_state = (u8)GPIO_ReadOutputDataBit(RELAY_PORT, RELAY_FAN_PIN);
}

static void Alarm_Output_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
                           RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    GPIO_ResetBits(ALARM_LED_PORT, ALARM_LED_PIN | ALARM_BEEP_PIN);
    gpio.GPIO_Pin = ALARM_LED_PIN | ALARM_BEEP_PIN;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(ALARM_LED_PORT, ALARM_LED_PIN | ALARM_BEEP_PIN);
}

static void Alarm_Output_Set(u8 active)
{
    if (active) GPIO_SetBits(ALARM_LED_PORT, ALARM_LED_PIN | ALARM_BEEP_PIN);
    else GPIO_ResetBits(ALARM_LED_PORT, ALARM_LED_PIN | ALARM_BEEP_PIN);
}

static u8 Parse_U32(const char *text, s32 *value)
{
    u8 index;
    u8 negative;
    s32 parsed;
    negative = 0U;
    parsed = 0;
    if (*text == '-' || *text == '+')
    {
        negative = (u8)(*text == '-');
        text++;
    }
    if (*text == '\0') return 0U;
    for (index = 0U; text[index] != '\0'; index++)
    {
        if (text[index] < '0' || text[index] > '9') return 0U;
        if (parsed > 500000L) return 0U;
        parsed = parsed * 10L + (s32)(text[index] - '0');
    }
    *value = negative ? -parsed : parsed;
    return 1U;
}

static u8 Settings_Apply(char key, const char *value)
{
    s32 parsed;
    if (!Parse_U32(value, &parsed)) return 0U;
    switch (key)
    {
        case 'A': settings.alarm_enabled = (u8)(parsed != 0); break;
        case 'W': settings.water_threshold = (u8)(parsed < 0 ? 0 : parsed); break;
        case 'C': settings.co2_threshold = (u16)(parsed < 0 ? 0 : parsed); break;
        case 'S': settings.soil_threshold = (u8)(parsed < 0 ? 0 : parsed); break;
        case 'T': settings.temp_threshold_x10 = (s16)parsed; break;
        case 'H': settings.humidity_threshold_x10 =
                         (u16)(parsed < 0 ? 0 : parsed); break;
        case 'M': settings.light_alarm_mode =
                         (u8)(parsed < 0 ? 0 : parsed); break;
        default: return 0U;
    }
    Settings_Clamp();
    if (key != 'A') Alarm_ResetStreaks();
    return 1U;
}

static void Build_Data_Line(char *line, u16 size,
                            const AgriculturePacket *packet)
{
    u16 water_raw;
    u16 soil_raw;
    s16 temperature_x10;
    u16 humidity_x10;
    int written;

    if (line == 0 || size == 0U) return;
    /*
     * The validity bits are authoritative.  Do not put the internal error
     * sentinels (-32768/65535) into fields whose cloud contract has normal
     * engineering ranges; one invalid SHT30 reading must not make the whole
     * HTTP telemetry request fail validation.
     */
    water_raw = packet->water_valid ? packet->water_raw : 0U;
    soil_raw = packet->soil_valid ? packet->soil_raw : 0U;
    temperature_x10 = packet->sht30_valid ? packet->temperature_x10 : 0;
    humidity_x10 = packet->sht30_valid ? packet->humidity_x10 : 0U;
    written = snprintf(line, size,
        "v=4;hostSeq=%u;slave=%u;seq=%u;waterRaw=%u;waterPct=%u;"
        "waterValid=%u;soilRaw=%u;soilPct=%u;soilValid=%u;co2Ppm=%u;"
        "co2Valid=%u;lux=%u;tempX10=%d;humidityX10=%u;sht30Valid=%u;"
        "gpsValid=0;latX10000=0;lonX10000=0;alarmEnabled=%u;"
        "waterThreshold=%u;co2Threshold=%u;soilThreshold=%u;"
        "tempThresholdX10=%d;humidityThresholdX10=%u;lightAlarmMode=%u;"
        "waterAlarm=%u;co2Alarm=%u;soilAlarm=%u;tempAlarm=%u;"
        "humidityAlarm=%u;lightAlarm=%u;pumpState=%u;lightState=%u;"
        "fanState=%u;alarmTest=%u",
        packet->sequence, SLAVE_ID, packet->sequence, water_raw,
        packet->water_pct, packet->water_valid, soil_raw,
        packet->soil_pct, packet->soil_valid, packet->co2_ppm,
        packet->co2_valid, packet->lux, temperature_x10,
        humidity_x10, packet->sht30_valid, packet->alarm_enabled,
        packet->water_threshold, packet->co2_threshold, packet->soil_threshold,
        packet->temp_threshold_x10, packet->humidity_threshold_x10,
        packet->light_alarm_mode, packet->water_alarm, packet->co2_alarm,
        packet->soil_alarm, packet->temp_alarm, packet->humidity_alarm,
        packet->light_alarm, packet->pump_state, packet->light_state,
        packet->fan_state, packet->alarm_test_active);
    /* Never upload a partial frame if fields grow in a future version. */
    if (written < 0 || (unsigned int)written >= size) line[0] = '\0';
}

static u8 Wifi_Field(const char *line, const char *key,
                     char *value, u16 value_size)
{
    const char *start = strstr(line, key);
    const char *end;
    u16 length;
    if (!start || start != line && start[-1] != ';') return 0U;
    start += strlen(key);
    if (*start != '=') return 0U;
    start++;
    end = strchr(start, ';');
    length = (u16)(end ? end - start : strlen(start));
    if (value_size == 0U) return 0U;
    if (length >= value_size) length = value_size - 1U;
    memcpy(value, start, length);
    value[length] = '\0';
    return 1U;
}

static u8 Wifi_ApplyCommand(AgriculturePacket *packet,
                             const char *command)
{
    char op[32];
    char value[24];
    char key_text[4];
    u8 ok = 0U;
    if (!Wifi_Field(command, "op", op, sizeof(op))) return 0U;
    if (!Wifi_Field(command, "value", value, sizeof(value))) value[0] = '\0';
    if (!strcmp(op, "set_alarm_enabled")) {
        strcpy(key_text, "A"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_water_threshold")) {
        strcpy(key_text, "W"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_co2_threshold")) {
        strcpy(key_text, "C"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_soil_threshold")) {
        strcpy(key_text, "S"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_temperature_threshold")) {
        strcpy(key_text, "T"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_humidity_threshold")) {
        strcpy(key_text, "H"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "set_light_alarm_mode")) {
        strcpy(key_text, "M"); ok = Settings_Apply(key_text[0], value);
    } else if (!strcmp(op, "pump_on") || !strcmp(op, "pump_off")) {
        manual_mask |= 0x01U; if (!strcmp(op, "pump_on")) manual_state |= 0x01U; else manual_state &= (u8)~0x01U; ok = 1U;
    } else if (!strcmp(op, "light_on") || !strcmp(op, "light_off")) {
        manual_mask |= 0x02U; if (!strcmp(op, "light_on")) manual_state |= 0x02U; else manual_state &= (u8)~0x02U; ok = 1U;
    } else if (!strcmp(op, "fan_on") || !strcmp(op, "fan_off")) {
        manual_mask |= 0x04U; if (!strcmp(op, "fan_on")) manual_state |= 0x04U; else manual_state &= (u8)~0x04U; ok = 1U;
    } else if (!strcmp(op, "alarm_test") || !strcmp(op, "relay_test")) {
        if (!strcmp(op, "alarm_test")) { alarm_test_ms = 3000U; packet->alarm_test_active = 1U; }
        else { relay_test_ms = 3000U; relay_test_step = 0U; }
        ok = 1U;
    }
    if (ok) {
        Packet_SyncSettings(packet);
        Sensor_UpdateAlarms(packet);
        Relay_Apply(packet);
    }
    return ok;
}

static void Sensor_Read(AgriculturePacket *packet, u16 sequence, u32 now_ms)
{
    static u16 cached_lux = SENSOR_LUX_ERROR;
    static s16 cached_temperature = SHT30_TEMPERATURE_ERROR;
    static u16 cached_humidity = SHT30_HUMIDITY_ERROR;
    static u32 next_lux_ms;
    static u32 next_sht30_ms;
    s16 temperature;
    u16 humidity;
    u8 i2c_ok;

    CO2_Service(now_ms);
    packet->sequence = sequence;
    packet->water_raw = Sensor_ADC_Read(WATER_ADC_CHANNEL);
    packet->soil_raw = Sensor_ADC_Read(SOIL_ADC_CHANNEL);
    packet->water_valid = (u8)(packet->water_raw != SENSOR_ADC_ERROR);
    packet->soil_valid = (u8)(packet->soil_raw != SENSOR_ADC_ERROR);
    packet->water_pct = Raw_To_Pct(packet->water_raw, WATER_RAW_EMPTY,
                                   WATER_RAW_FULL, 0U);
    packet->soil_pct = Raw_To_Pct(packet->soil_raw, SOIL_RAW_WET,
                                  SOIL_RAW_DRY, 1U);
    packet->co2_ppm = co2_ppm;
    packet->co2_valid = co2_valid;

    i2c_ok = 1U;
    if ((s32)(now_ms - next_lux_ms) >= 0)
    {
        cached_lux = BH1750_ReadLux();
        if (cached_lux == SENSOR_LUX_ERROR) i2c_ok = 0U;
        next_lux_ms = now_ms + 3000UL;
    }
    packet->lux = cached_lux;
    if ((s32)(now_ms - next_sht30_ms) >= 0)
    {
        if (SHT30_Read(&temperature, &humidity))
        {
            cached_temperature = temperature;
            cached_humidity = humidity;
        }
        else
        {
            cached_temperature = SHT30_TEMPERATURE_ERROR;
            cached_humidity = SHT30_HUMIDITY_ERROR;
            i2c_ok = 0U;
        }
        next_sht30_ms = now_ms + 5000UL;
    }
    if (!i2c_ok)
    {
        if (sensor_i2c_error_streak < 255U) sensor_i2c_error_streak++;
        if (sensor_i2c_error_streak >= 3U)
        {
            I2C_BusRecover();
            I2C_WriteCommand(BH1750_POWER_ON);
            I2C_WriteCommand(BH1750_RESET);
            sensor_i2c_error_streak = 0U;
        }
    }
    else if (sensor_i2c_error_streak > 0U)
    {
        sensor_i2c_error_streak--;
    }
    packet->temperature_x10 = cached_temperature;
    packet->humidity_x10 = cached_humidity;
    packet->sht30_valid = (u8)(cached_temperature != SHT30_TEMPERATURE_ERROR &&
                               cached_humidity != SHT30_HUMIDITY_ERROR);
    Packet_SyncSettings(packet);
    Sensor_UpdateAlarms(packet);
}

static void Text_U16(char *output, u16 value)
{
    sprintf(output, "%u", value);
}

static void Text_S16X10(char *output, s16 value)
{
    s16 integer;
    u16 fraction;
    integer = (s16)(value / 10);
    fraction = (u16)(value < 0 ? -(value % 10) : value % 10);
    sprintf(output, "%d.%u", integer, fraction);
}

static void Text_U16X10(char *output, u16 value)
{
    sprintf(output, "%u.%u", (u16)(value / 10U),
            (u16)(value % 10U));
}

static u8 Slave_ShowMenu12(u8 x, u8 y, const u8 *text, u8 count)
{
    u8 index;

    for (index = 0U; index < count && x <= 116U; index++)
    {
        OLED_ShowChinese12(x, y, menu_hz16[text[index]]);
        x = (u8)(x + 12U);
    }
    return x;
}

static void Key_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin = KEY1_PIN | KEY2_PIN | KEY3_PIN | KEY4_PIN;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(KEY_PORT, &gpio);
}

static u8 Key_Update(u16 pin, u8 *sample_state,
                     u8 *stable_state, u16 *counter_ms)
{
    u8 sample;

    sample = (u8)(GPIO_ReadInputDataBit(KEY_PORT, pin) ==
                  KEY_ACTIVE_LEVEL);
    if (sample != *sample_state)
    {
        *sample_state = sample;
        *counter_ms = 0U;
    }
    else if (*counter_ms < KEY_DEBOUNCE_MS)
    {
        (*counter_ms)++;
        if (*counter_ms >= KEY_DEBOUNCE_MS &&
            *stable_state != *sample_state)
        {
            *stable_state = *sample_state;
            if (*stable_state) return 1U;
        }
    }
    return 0U;
}

static u8 Slave_ShowHz14(u8 x, u8 y, const u8 *text, u8 count)
{
    u8 index;

    for (index = 0U; index < count && x <= 114U; index++)
    {
        OLED_ShowChinese14(x, y, slave_hz16[text[index]]);
        x = (u8)(x + 14U);
    }
    return x;
}

static void Slave_ShowBitmapValue14(u8 y,
                                    const u8 *label0,
                                    const u8 *label1,
                                    const u8 *label2,
                                    const u8 *label3,
                                    u8 count,
                                    const char *value)
{
    u8 x;

    x = 0U;
    if (count > 0U) { OLED_ShowChinese14(x, y, label0); x += 14U; }
    if (count > 1U) { OLED_ShowChinese14(x, y, label1); x += 14U; }
    if (count > 2U) { OLED_ShowChinese14(x, y, label2); x += 14U; }
    if (count > 3U) { OLED_ShowChinese14(x, y, label3); x += 14U; }
    OLED_ShowColon14(x, y);
    x = (u8)(x + 14U);
    OLED_ShowAscii16(x, y, value);
}

static void Slave_ShowRelay14(u8 y, const u8 *label0, const u8 *label1,
                              u8 active)
{
    OLED_ShowChinese14(0U, y, label0);
    OLED_ShowChinese14(14U, y, label1);
    OLED_ShowColon14(28U, y);
    OLED_ShowChinese14(42U, y, active ? agri_hz_kai : agri_hz_guan);
}

static void Slave_Draw(const AgriculturePacket *packet,
                       u8 config_result,
                       u16 silence_ms, u8 page)
{
    char number[20];
    u8 online;
    u8 alarm;
    u8 display_page;
    u8 x;
    static u8 first_draw = 1U;
    static u8 last_page = 0xFFU;
    static u8 last_online;
    static u8 last_alarm;
    static u8 last_water;
    static u8 last_soil;
    static u16 last_co2;
    static u16 last_lux;
    static s16 last_temperature;
    static u16 last_humidity;
    static u8 last_water_valid;
    static u8 last_soil_valid;
    static u8 last_co2_valid;
    static u8 last_sht30_valid;
    static u8 last_pump;
    static u8 last_light;
    static u8 last_fan;

    online = (u8)(config_result == WIFI_CONFIG_OK &&
                  silence_ms < 5000U);
    display_page = (u8)(page % SLAVE_DISPLAY_PAGE_COUNT);
    alarm = (u8)(packet->water_alarm || packet->co2_alarm ||
                 packet->soil_alarm || packet->temp_alarm ||
                 packet->humidity_alarm || packet->alarm_test_active);

    if (!slave_screen_force && !first_draw && last_page == display_page &&
        last_online == online && last_alarm == alarm &&
        last_water == packet->water_pct && last_soil == packet->soil_pct &&
        last_co2 == packet->co2_ppm && last_lux == packet->lux &&
        last_temperature == packet->temperature_x10 &&
        last_humidity == packet->humidity_x10 &&
        last_water_valid == packet->water_valid &&
        last_soil_valid == packet->soil_valid &&
        last_co2_valid == packet->co2_valid &&
        last_sht30_valid == packet->sht30_valid &&
        last_pump == packet->pump_state && last_light == packet->light_state &&
        last_fan == packet->fan_state)
        return;

    OLED_Clear();
    if (display_page == 0U)
    {
        x = Slave_ShowHz14(0U, 0U, ui_comm_label, 2U);
        OLED_ShowColon14(x, 0U);
        x = (u8)(x + 14U);
        Slave_ShowHz14(x, 0U,
                       online ? ui_connected_text : ui_disconnected_text,
                       3U);

        if (!packet->water_valid) strcpy(number, "--");
        else { Text_U16(number, packet->water_pct); strcat(number, "%"); }
        Slave_ShowBitmapValue14(16U, agri_hz_shui, agri_hz_wei,
                                0, 0, 2U, number);

        if (!packet->co2_valid) strcpy(number, "--");
        else { Text_U16(number, packet->co2_ppm); strcat(number, "ppm"); }
        Slave_ShowBitmapValue14(32U, agri_hz_er, agri_hz_yang,
                                agri_hz_hua, agri_hz_tan, 4U, number);

        Slave_ShowHz14(0U, 48U, ui_alarm_label, 2U);
        OLED_ShowColon14(28U, 48U);
        if (alarm)
        {
            OLED_ShowChinese14(42U, 48U, agri_hz_yi_abnormal);
            OLED_ShowChinese14(56U, 48U, agri_hz_chang);
        }
        else
        {
            OLED_ShowChinese14(42U, 48U, agri_hz_zheng);
            OLED_ShowChinese14(56U, 48U, agri_hz_chang);
        }
    }
    else if (display_page == 1U)
    {
        if (!packet->soil_valid) strcpy(number, "--");
        else { Text_U16(number, packet->soil_pct); strcat(number, "%"); }
        Slave_ShowBitmapValue14(0U, agri_hz_tu, agri_hz_rang,
                                sht30_hz_shi,
                                agri_hz_du, 4U, number);

        if (!packet->sht30_valid) strcpy(number, "--");
        else { Text_S16X10(number, packet->temperature_x10); strcat(number, "C"); }
        Slave_ShowBitmapValue14(16U, agri_hz_huan, agri_hz_jing,
                                sht30_hz_wen, sht30_hz_du, 4U, number);

        if (!packet->sht30_valid) strcpy(number, "--");
        else { Text_U16X10(number, packet->humidity_x10); strcat(number, "%"); }
        Slave_ShowBitmapValue14(32U, agri_hz_huan, agri_hz_jing,
                                sht30_hz_shi, sht30_hz_du, 4U, number);

        if (packet->lux == SENSOR_LUX_ERROR)
        {
            strcpy(number, "--");
            Slave_ShowBitmapValue14(48U, agri_hz_guang, agri_hz_zhao,
                                    0, 0, 2U, number);
        }
        else
        {
            u8 light_state;

            light_state = Lux_State(packet->lux);
            OLED_ShowChinese14(0U, 48U, agri_hz_guang);
            OLED_ShowChinese14(14U, 48U, agri_hz_zhao);
            x = 28U;
            OLED_ShowColon14(x, 48U);
            x = (u8)(x + 14U);
            if (light_state == LIGHT_ALARM_DARK)
            {
                OLED_ShowChinese14(x, 48U, slave_hz16[SLAVE_HZ_AN]);
            }
            else
            {
                OLED_ShowChinese14(x, 48U,
                                   light_state == LIGHT_ALARM_LOW ?
                                   slave_hz16[SLAVE_HZ_DI] :
                                   slave_hz16[SLAVE_HZ_GAO]);
                x = (u8)(x + 14U);
                OLED_ShowChinese14(x, 48U, agri_hz_guang);
                x = (u8)(x + 14U);
                OLED_ShowChinese14(x, 48U, agri_hz_zhao);
            }
        }
    }
    else if (display_page == 2U)
    {
        Slave_ShowRelay14(0U, agri_hz_shui, agri_hz_beng,
                          packet->pump_state);
        Slave_ShowRelay14(16U, agri_hz_guang,
                          agri_hz_deng, packet->light_state);
        Slave_ShowRelay14(32U, agri_hz_feng, agri_hz_shan,
                          packet->fan_state);
        Slave_ShowHz14(0U, 48U, ui_status_label, 2U);
        OLED_ShowColon14(28U, 48U);
        if (alarm)
        {
            OLED_ShowChinese14(42U, 48U, agri_hz_yi_abnormal);
            OLED_ShowChinese14(56U, 48U, agri_hz_chang);
        }
        else
        {
            OLED_ShowChinese14(42U, 48U, agri_hz_zheng);
            OLED_ShowChinese14(56U, 48U, agri_hz_chang);
        }
    }
    OLED_Update();
    first_draw = 0U;
    last_page = display_page;
    last_online = online;
    last_alarm = alarm;
    last_water = packet->water_pct;
    last_soil = packet->soil_pct;
    last_co2 = packet->co2_ppm;
    last_lux = packet->lux;
    last_temperature = packet->temperature_x10;
    last_humidity = packet->humidity_x10;
    last_water_valid = packet->water_valid;
    last_soil_valid = packet->soil_valid;
    last_co2_valid = packet->co2_valid;
    last_sht30_valid = packet->sht30_valid;
    last_pump = packet->pump_state;
    last_light = packet->light_state;
    last_fan = packet->fan_state;
    slave_screen_force = 0U;
}

static void Settings_ShowLabel12(u8 x, u8 y,
                                 const u8 *const glyphs[], u8 count)
{
    u8 index;

    for (index = 0U; index < count && x <= 116U; index++)
    {
        OLED_ShowChinese12(x, y, glyphs[index]);
        x = (u8)(x + 12U);
    }
    OLED_ShowColon12(x, y);
}

static void Slave_DrawSettings(u8 selected_item)
{
    char value[16];
    const u8 *glyphs[4];
    const u8 *light_value;
    static const u8 cursor_y[4] = {8U, 24U, 32U, 48U};
    u8 first_item;
    u8 item;
    u8 row;
    u8 y;
    u8 count;

    OLED_Clear();
    /* Keep the legacy V3 settings header and rhythm: centered title,
       four 12px rows, and the same 16px selection chevron as V3. */
    Slave_ShowMenu12(38U, 0U, ui_settings_title, 4U);
    first_item = selected_item < 4U ? 0U : 4U;

    for (row = 0U; row < 4U; row++)
    {
        item = (u8)(first_item + row);
        if (item >= 7U) break;
        y = (u8)(13U + row * 13U);
        if (selected_item == item)
            OLED_ShowAscii16(0U, cursor_y[row], ">");
        count = 0U;
        value[0] = '\0';

        if (item == 0U)
        {
            glyphs[count++] = slave_hz16[SLAVE_HZ_BAO];
            glyphs[count++] = slave_hz16[SLAVE_HZ_JING];
            light_value = settings.alarm_enabled ?
                          menu_hz16[MENU_HZ_KAI] : menu_hz16[MENU_HZ_GUAN];
            Settings_ShowLabel12(9U, y, glyphs, count);
            OLED_ShowChinese12(45U, y, light_value);
        }
        else if (item == 1U)
        {
            glyphs[count++] = agri_hz_shui;
            glyphs[count++] = agri_hz_wei;
            Settings_ShowLabel12(9U, y, glyphs, count);
            sprintf(value, "%u%%", settings.water_threshold);
            OLED_ShowAscii8(45U, y, value);
        }
        else if (item == 2U)
        {
            glyphs[count++] = agri_hz_er;
            glyphs[count++] = agri_hz_yang;
            glyphs[count++] = agri_hz_hua;
            glyphs[count++] = agri_hz_tan;
            Settings_ShowLabel12(9U, y, glyphs, count);
            sprintf(value, "%uppm", settings.co2_threshold);
            OLED_ShowAscii8(69U, y, value);
        }
        else if (item == 3U)
        {
            glyphs[count++] = agri_hz_tu;
            glyphs[count++] = agri_hz_rang;
            glyphs[count++] = sht30_hz_shi;
            glyphs[count++] = agri_hz_du;
            Settings_ShowLabel12(9U, y, glyphs, count);
            sprintf(value, "%u%%", settings.soil_threshold);
            OLED_ShowAscii8(69U, y, value);
        }
        else if (item == 4U)
        {
            glyphs[count++] = agri_hz_huan;
            glyphs[count++] = agri_hz_jing;
            glyphs[count++] = sht30_hz_wen;
            glyphs[count++] = agri_hz_du;
            Settings_ShowLabel12(9U, y, glyphs, count);
            Text_S16X10(value, settings.temp_threshold_x10);
            strcat(value, "C");
            OLED_ShowAscii8(69U, y, value);
        }
        else if (item == 5U)
        {
            glyphs[count++] = agri_hz_huan;
            glyphs[count++] = agri_hz_jing;
            glyphs[count++] = sht30_hz_shi;
            glyphs[count++] = agri_hz_du;
            Settings_ShowLabel12(9U, y, glyphs, count);
            Text_U16X10(value, settings.humidity_threshold_x10);
            strcat(value, "%");
            OLED_ShowAscii8(69U, y, value);
        }
        else
        {
            glyphs[count++] = agri_hz_guang;
            glyphs[count++] = agri_hz_zhao;
            glyphs[count++] = slave_hz16[SLAVE_HZ_BAO];
            glyphs[count++] = slave_hz16[SLAVE_HZ_JING];
            Settings_ShowLabel12(9U, y, glyphs, count);
            light_value = settings.light_alarm_mode == LIGHT_ALARM_DARK ?
                          slave_hz16[SLAVE_HZ_AN] :
                          (settings.light_alarm_mode == LIGHT_ALARM_LOW ?
                           slave_hz16[SLAVE_HZ_DI] :
                           slave_hz16[SLAVE_HZ_GAO]);
            OLED_ShowChinese12(69U, y, light_value);
        }
    }
    OLED_Update();
}

static void Slave_AdjustSetting(AgriculturePacket *packet,
                                u8 item, u8 increase)
{
    s32 value;

    if (item == 0U)
        settings.alarm_enabled = (u8)!settings.alarm_enabled;
    else if (item == 1U)
    {
        value = (s32)settings.water_threshold +
                (increase ? 5 : -5);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        settings.water_threshold = (u8)value;
    }
    else if (item == 2U)
    {
        value = (s32)settings.co2_threshold +
                (increase ? 100 : -100);
        if (value < 0) value = 0;
        if (value > 50000) value = 50000;
        settings.co2_threshold = (u16)value;
    }
    else if (item == 3U)
    {
        value = (s32)settings.soil_threshold +
                (increase ? 5 : -5);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        settings.soil_threshold = (u8)value;
    }
    else if (item == 4U)
    {
        value = (s32)settings.temp_threshold_x10 +
                (increase ? 5 : -5);
        if (value < -400) value = -400;
        if (value > 1250) value = 1250;
        settings.temp_threshold_x10 = (s16)value;
    }
    else if (item == 5U)
    {
        value = (s32)settings.humidity_threshold_x10 +
                (increase ? 5 : -5);
        if (value < 0) value = 0;
        if (value > 1000) value = 1000;
        settings.humidity_threshold_x10 = (u16)value;
    }
    else
    {
        if (increase)
            settings.light_alarm_mode =
                (u8)((settings.light_alarm_mode + 1U) % 3U);
        else
            settings.light_alarm_mode =
                settings.light_alarm_mode == 0U ? 2U :
                (u8)(settings.light_alarm_mode - 1U);
    }
    Settings_Clamp();
    if (item != 0U) Alarm_ResetStreaks();
    Packet_SyncSettings(packet);
    Sensor_UpdateAlarms(packet);
    Relay_Apply(packet);
}

int main(void)
{
    static AgriculturePacket packet;
    u8 config_result;
    static char receive_line[WIFI_COMMAND_BUFFER_SIZE];
    /* Main never returns; keep long-lived data out of the nested call stack. */
    static char transmit_line[WIFI_TELEMETRY_SIZE];
    u32 wifi_command_id;
    u16 sensor_timer;
    u16 draw_timer;
    u16 silence_ms;
    u16 alarm_tick_ms;
    u8 alarm_phase;
    u32 uptime_ms;
    u16 relay_tick;
    u8 display_page;
    u8 ui_mode;
    u8 settings_item;
    u8 key1_sample;
    u8 key1_stable;
    u8 key2_sample;
    u8 key2_stable;
    u8 key3_sample;
    u8 key3_stable;
    u8 key4_sample;
    u8 key4_stable;
    u8 key1_event;
    u8 key2_event;
    u8 key3_event;
    u8 key4_event;
    u16 key1_counter;
    u16 key2_counter;
    u16 key3_counter;
    u16 key4_counter;

    SystemInit();
    delay_init(72U);
    Alarm_Output_Init();
    Relay_Init();
    Key_Init();
    OLED_Init();
    Sensor_Init();
    CO2_Init();
    Settings_Reset();
    Watchdog_Init();

    memset(&packet, 0, sizeof(packet));
    packet.lux = SENSOR_LUX_ERROR;
    packet.temperature_x10 = SHT30_TEMPERATURE_ERROR;
    packet.humidity_x10 = SHT30_HUMIDITY_ERROR;
    Packet_SyncSettings(&packet);
    manual_mask = 0U;
    manual_state = 0U;
    relay_test_ms = 0U;
    alarm_test_ms = 0U;
    relay_test_step = 0U;
    Relay_Apply(&packet);

    WifiRelay_Init();
    config_result = WIFI_CONFIG_OK;
    sensor_timer = 10U;
    draw_timer = 0U;
    silence_ms = 60000U;
    alarm_tick_ms = 0U;
    alarm_phase = 0U;
    uptime_ms = 0UL;
    relay_tick = 0U;
    display_page = 0U;
    ui_mode = 0U;
    settings_item = 0U;
    sensor_alarm_armed = 0U;
    alarm_sequence_seen = 0U;
    alarm_last_sequence = 0U;
    Alarm_ResetStreaks();
    slave_screen_force = 1U;
    key1_sample = 0U; key1_stable = 0U; key1_counter = 0U;
    key2_sample = 0U; key2_stable = 0U; key2_counter = 0U;
    key3_sample = 0U; key3_stable = 0U; key3_counter = 0U;
    key4_sample = 0U; key4_stable = 0U; key4_counter = 0U;
    Slave_Draw(&packet, config_result, silence_ms, display_page);

    while (1)
    {
        Watchdog_Feed();
        Build_Data_Line(transmit_line, sizeof(transmit_line), &packet);
        WifiRelay_Service(transmit_line);
        if (WifiRelay_IsOnline()) silence_ms = 0U;
        if (WifiRelay_GetCommand(receive_line, sizeof(receive_line), &wifi_command_id))
        {
            if (Wifi_ApplyCommand(&packet, receive_line))
                WifiRelay_SendAck(wifi_command_id, "ok");
            else
                WifiRelay_SendAck(wifi_command_id, "unsupported");
        }
#if 0
        while (LoRa_ReadByte(&receive_data))
        {
            Watchdog_Feed();
            if (receive_data == '\r' || receive_data == '\n')
            {
                if (receive_length != 0U)
                {
                    receive_line[receive_length] = '\0';
                    if (receive_length == 11U &&
                        Parse_Request_Line(receive_line, &target_id,
                                           &request_sequence) &&
                        (target_id == 0U || target_id == SLAVE_ID))
                    {
                        packet.sequence = request_sequence;
                        Build_Data_Line(transmit_line,
                                        sizeof(transmit_line), &packet);
                        if (config_result == LORA_CONFIG_OK &&
                            LoRa_SendPacket(transmit_line))
                            silence_ms = 0U;
                    }
                    else if (Parse_Set_Line(receive_line, &target_id, &key,
                                            &value) &&
                             (target_id == 0U || target_id == SLAVE_ID))
                    {
                        if ((key == 'X' || key == 'R') &&
                            value[0] == '1' && value[1] == '\0')
                        {
                            if (key == 'X')
                            {
                                alarm_test_ms = 3000U;
                                packet.alarm_test_active = 1U;
                            }
                            else
                            {
                                relay_test_ms = 3000U;
                                relay_test_step = 0U;
                            }
                            Build_Ack_Line(transmit_line,
                                           sizeof(transmit_line), key, 1);
                            if (config_result == LORA_CONFIG_OK)
                                LoRa_SendPacket(transmit_line);
                        }
                        else if ((key == 'P' || key == 'I' || key == 'F') &&
                                 (value[0] == '0' || value[0] == '1') &&
                                 value[1] == '\0')
                        {
                            u8 bit;
                            u8 enabled;
                            bit = (key == 'P') ? 0x01U :
                                  ((key == 'I') ? 0x02U : 0x04U);
                            enabled = (u8)(value[0] == '1');
                            manual_mask |= bit;
                            if (enabled) manual_state |= bit;
                            else manual_state &= (u8)~bit;
                            Relay_Apply(&packet);
                            Build_Ack_Line(transmit_line,
                                           sizeof(transmit_line), key,
                                           enabled);
                            if (config_result == LORA_CONFIG_OK)
                            {
                                LoRa_SendPacket(transmit_line);
                                Build_Data_Line(transmit_line,
                                                sizeof(transmit_line), &packet);
                                LoRa_SendPacket(transmit_line);
                            }
                        }
                        else if (Settings_Apply(key, value))
                        {
                            Packet_SyncSettings(&packet);
                            Sensor_UpdateAlarms(&packet);
                            Relay_Apply(&packet);
                            Build_Ack_Line(transmit_line,
                                           sizeof(transmit_line), key,
                                           (key == 'T') ?
                                           settings.temp_threshold_x10 :
                                           (key == 'H' ?
                                            settings.humidity_threshold_x10 :
                                            (key == 'C' ?
                                             settings.co2_threshold :
                                             (key == 'W' ?
                                              settings.water_threshold :
                                              (key == 'S' ?
                                               settings.soil_threshold :
                                               settings.light_alarm_mode)))));
                            if (config_result == LORA_CONFIG_OK)
                            {
                                LoRa_SendPacket(transmit_line);
                                Build_Data_Line(transmit_line,
                                                sizeof(transmit_line), &packet);
                                LoRa_SendPacket(transmit_line);
                            }
                        }
                    }
                    receive_length = 0U;
                }
            }
            else if (receive_data >= 32U && receive_data <= 126U)
            {
                if (receive_length < sizeof(receive_line) - 1U)
                    receive_line[receive_length++] = (char)receive_data;
                else
                    receive_length = 0U;
            }
        }
#endif

        Safe_DelayMs(1U);
        uptime_ms++;
        if (!sensor_alarm_armed && uptime_ms >= ALARM_STARTUP_GUARD_MS)
        {
            sensor_alarm_armed = 1U;
            slave_screen_force = 1U;
        }
        if (silence_ms < 60000U) silence_ms++;
        if (sensor_timer > 0U) sensor_timer--;
        if (draw_timer < 60000U) draw_timer++;

        CO2_Service(uptime_ms);
        if (sensor_timer == 0U)
        {
            sensor_timer = SENSOR_REFRESH_MS;
            packet.sequence++;
            if (packet.sequence > 9999U) packet.sequence = 1U;
            Sensor_Read(&packet, packet.sequence, uptime_ms);
            if (relay_test_ms == 0U) Relay_Apply(&packet);
            Watchdog_Feed();
        }

        if (relay_test_ms > 0U)
        {
            relay_test_ms--;
            relay_tick++;
            if (relay_tick >= 1000U)
            {
                relay_tick = 0U;
                relay_test_step++;
                if (relay_test_step >= 3U) relay_test_step = 0U;
            }
            Relay_ApplyTest(&packet);
            if (relay_test_ms == 0U) Relay_Apply(&packet);
        }
        if (alarm_test_ms > 0U)
        {
            alarm_test_ms--;
            packet.alarm_test_active = 1U;
            Alarm_Output_Set(1U);
        }
        else if (settings.alarm_enabled &&
                 (packet.water_alarm || packet.co2_alarm ||
                  packet.soil_alarm || packet.temp_alarm ||
                  packet.humidity_alarm))
        {
            alarm_tick_ms++;
            if (alarm_tick_ms >= 250U)
            {
                alarm_tick_ms = 0U;
                alarm_phase = (u8)!alarm_phase;
            }
            Alarm_Output_Set(alarm_phase);
            packet.alarm_test_active = 0U;
        }
        else
        {
            alarm_tick_ms = 0U;
            alarm_phase = 0U;
            packet.alarm_test_active = 0U;
            Alarm_Output_Set(0U);
        }

        key1_event = Key_Update(KEY1_PIN, &key1_sample,
                                &key1_stable, &key1_counter);
        key2_event = Key_Update(KEY2_PIN, &key2_sample,
                                &key2_stable, &key2_counter);
        key3_event = Key_Update(KEY3_PIN, &key3_sample,
                                &key3_stable, &key3_counter);
        key4_event = Key_Update(KEY4_PIN, &key4_sample,
                                &key4_stable, &key4_counter);
        if (!ui_mode)
        {
            if (key1_event)
            {
                display_page = display_page == 0U ?
                               (SLAVE_DISPLAY_PAGE_COUNT - 1U) :
                               (u8)(display_page - 1U);
                Slave_Draw(&packet, config_result, silence_ms,
                           display_page);
            }
            else if (key2_event)
            {
                display_page = (u8)((display_page + 1U) %
                                    SLAVE_DISPLAY_PAGE_COUNT);
                Slave_Draw(&packet, config_result, silence_ms,
                           display_page);
            }
            else if (key3_event)
            {
                ui_mode = 1U;
                settings_item = 0U;
                Slave_DrawSettings(settings_item);
            }
            else if (key4_event)
            {
                display_page = 0U;
                Slave_Draw(&packet, config_result, silence_ms,
                           display_page);
            }
        }
        else if (key4_event)
        {
            ui_mode = 0U;
            slave_screen_force = 1U;
            Slave_Draw(&packet, config_result, silence_ms,
                       display_page);
        }
        else if (key3_event)
        {
            settings_item++;
            if (settings_item >= 7U) settings_item = 0U;
            Slave_DrawSettings(settings_item);
        }
        else if (key1_event || key2_event)
        {
            Slave_AdjustSetting(&packet, settings_item,
                                key2_event ? 1U : 0U);
            Slave_DrawSettings(settings_item);
        }

        if (silence_ms >= RADIO_REINIT_MS)
            silence_ms = 60000U;
        if (draw_timer >= DISPLAY_REFRESH_MS)
        {
            draw_timer = 0U;
            if (ui_mode) Slave_DrawSettings(settings_item);
            else Slave_Draw(&packet, config_result, silence_ms,
                            display_page);
        }
    }
}
