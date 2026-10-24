#include "stm32f10x.h"
#include "stm32f10x_usart.h"
#include "misc.h"
#include "LoRa.h"
#include "delay.h"

static u32 lora_detected_baud;
static u8 lora_detected_parity;
static u8 lora_last_response_length;
static char lora_last_response_text[13];
#define LORA_RX_BUFFER_SIZE 256
static volatile u8 lora_rx_buffer[LORA_RX_BUFFER_SIZE];
static volatile u8 lora_rx_head;
static volatile u8 lora_rx_tail;
static volatile u16 lora_rx_overflow;

extern void Watchdog_Feed(void);

static void LoRa_SafeDelayMs(u16 nms)
{
    while (nms != 0U)
    {
        delay_ms(1);
        Watchdog_Feed();
        nms--;
    }
}

static void LoRa_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_2;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = LORA_MD0_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(LORA_MD0_PORT, &gpio);
    GPIO_SetBits(LORA_MD0_PORT, LORA_MD0_PIN);

    gpio.GPIO_Pin = LORA_AUX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(LORA_AUX_PORT, &gpio);
}

static void LoRa_UART_Set(u32 baudrate, u8 parity)
{
    USART_InitTypeDef usart;
    volatile u16 clear;

    USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);
    USART_Cmd(USART2, DISABLE);
    USART_DeInit(USART2);
    usart.USART_BaudRate = baudrate;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

    if (parity == 1)
    {
        usart.USART_WordLength = USART_WordLength_9b;
        usart.USART_Parity = USART_Parity_Even;
    }
    else if (parity == 2)
    {
        usart.USART_WordLength = USART_WordLength_9b;
        usart.USART_Parity = USART_Parity_Odd;
    }
    else
    {
        usart.USART_WordLength = USART_WordLength_8b;
        usart.USART_Parity = USART_Parity_No;
    }

    USART_Init(USART2, &usart);
    USART_Cmd(USART2, ENABLE);
    clear = USART2->SR;
    clear = USART2->DR;
    (void)clear;
    lora_rx_head = 0;
    lora_rx_tail = 0;
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
}

static void LoRa_ClearRx(void)
{
    volatile u16 clear;
    clear = USART2->SR;
    clear = USART2->DR;
    (void)clear;
    lora_rx_head = 0;
    lora_rx_tail = 0;
}

static u8 LoRa_SendByte(u8 data)
{
    u32 timeout;

    timeout = 60000UL;
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
    {
        if (timeout == 0) return 0;
        Watchdog_Feed();
        timeout--;
    }
    USART_SendData(USART2, data);
    return 1;
}

static u8 LoRa_SendRaw(const char *text)
{
    u32 timeout;

    while (*text != '\0')
    {
        if (!LoRa_SendByte((u8)*text)) return 0;
        text++;
    }
    timeout = 60000UL;
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
        if (timeout == 0) return 0;
        Watchdog_Feed();
        timeout--;
    }
    return 1;
}

static u8 LoRa_SendBuffer(const u8 *data, u8 length)
{
    u8 index;
    u32 timeout;

    for (index = 0; index < length; index++)
    {
        if (!LoRa_SendByte(data[index])) return 0;
    }
    timeout = 60000UL;
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
        if (timeout == 0) return 0;
        Watchdog_Feed();
        timeout--;
    }
    return 1;
}

static u8 LoRa_WaitIdle(u16 timeout_ms)
{
    while (timeout_ms != 0)
    {
        if (LoRa_AuxLevel() == 0) return 1;
        LoRa_SafeDelayMs(1);
        timeout_ms--;
    }
    return (LoRa_AuxLevel() == 0);
}

static u8 LoRa_Command(const char *command, u16 timeout_ms)
{
    u8 data;
    u8 previous;
    u8 text_index;
    u32 ticks;

    LoRa_WaitIdle(500);
    LoRa_SafeDelayMs(5);
    LoRa_ClearRx();
    lora_last_response_length = 0;
    lora_last_response_text[0] = '\0';
    text_index = 0;
    previous = 0;
    if (!LoRa_SendRaw(command)) return 0;
    ticks = (u32)timeout_ms * 20;

    while (ticks != 0)
    {
        if (LoRa_ReadByte(&data))
        {
            if (lora_last_response_length < 255) lora_last_response_length++;
            if (text_index < (sizeof(lora_last_response_text) - 1))
            {
                if (data == '\r' || data == '\n') lora_last_response_text[text_index++] = '.';
                else if (data >= 32 && data <= 126) lora_last_response_text[text_index++] = (char)data;
                else lora_last_response_text[text_index++] = '?';
                lora_last_response_text[text_index] = '\0';
            }
            if (previous == 'O' && data == 'K') return 1;
            previous = data;
        }
        delay_us(50);
        if ((ticks & 0x3FU) == 0U) Watchdog_Feed();
        ticks--;
    }
    return 0;
}

static u8 LoRa_CommandRetry(const char *command, u16 timeout_ms, u8 retries)
{
    u8 attempt;

    for (attempt = 0; attempt < retries; attempt++)
    {
        if (LoRa_Command(command, timeout_ms)) return 1;
        LoRa_SafeDelayMs(100);
    }
    return 0;
}

static u8 LoRa_DetectUART(void)
{
    static const u32 baudrates[] =
    {
        9600, 115200, 57600, 38400, 19200, 4800, 2400, 1200
    };
    u8 parity;
    u8 index;

    for (parity = 0; parity < 3; parity++)
    {
        for (index = 0; index < 8; index++)
        {
            LoRa_UART_Set(baudrates[index], parity);
            LoRa_SafeDelayMs(5);
            if (LoRa_Command("AT\r\n", 150))
            {
                lora_detected_baud = baudrates[index];
                lora_detected_parity = parity;
                return 1;
            }
        }
    }
    return 0;
}

void LoRa_Init(void)
{
    NVIC_InitTypeDef nvic;

    LoRa_GPIO_Init();
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    nvic.NVIC_IRQChannel = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 1;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    LoRa_UART_Set(LORA_RUN_BAUDRATE, 0);
    lora_detected_baud = 0;
    lora_detected_parity = 0;
    lora_last_response_length = 0;
    lora_rx_overflow = 0;
    LoRa_SafeDelayMs(300);
}

LoRaConfigResult LoRa_Configure(void)
{
    LoRaConfigResult result;

    result = LORA_CONFIG_OK;
    GPIO_SetBits(LORA_MD0_PORT, LORA_MD0_PIN);
    LoRa_SafeDelayMs(300);

    if (!LoRa_WaitIdle(3000)) result = LORA_CONFIG_AUX_TIMEOUT;
    else if (!LoRa_DetectUART()) result = LORA_CONFIG_AT_NOT_FOUND;
    else
    {
        lora_rx_overflow = 0;
        LoRa_SafeDelayMs(100);
        if (!LoRa_CommandRetry("AT\r\n", 800, 3)) result = LORA_CONFIG_DEFAULT_FAILED;
        else if (!LoRa_CommandRetry("AT+ADDR=00,00\r\n", 800, 3)) result = LORA_CONFIG_ADDR_FAILED;
        else if (!LoRa_CommandRetry("AT+WLRATE=23,5\r\n", 800, 3)) result = LORA_CONFIG_RADIO_FAILED;
        else if (!LoRa_CommandRetry("AT+TPOWER=3\r\n", 800, 3)) result = LORA_CONFIG_POWER_FAILED;
        else if (!LoRa_CommandRetry("AT+CWMODE=0\r\n", 800, 3)) result = LORA_CONFIG_MODE_FAILED;
        else if (!LoRa_CommandRetry("AT+TMODE=0\r\n", 800, 3)) result = LORA_CONFIG_TMODE_FAILED;
        else if (!LoRa_CommandRetry("AT+WLTIME=0\r\n", 800, 3)) result = LORA_CONFIG_SLEEP_FAILED;
        else if (!LoRa_CommandRetry("AT+UART=7,0\r\n", 800, 3)) result = LORA_CONFIG_UART_FAILED;
    }

    LoRa_UART_Set(LORA_RUN_BAUDRATE, 0);
    LoRa_ClearRx();
    GPIO_ResetBits(LORA_MD0_PORT, LORA_MD0_PIN);
    LoRa_SafeDelayMs(30);
    LoRa_WaitIdle(1000);
    return result;
}

const char *LoRa_ConfigResultText(LoRaConfigResult result)
{
    switch (result)
    {
        case LORA_CONFIG_OK:             return "OK";
        case LORA_CONFIG_AUX_TIMEOUT:    return "AUX TIMEOUT";
        case LORA_CONFIG_AT_NOT_FOUND:   return "NO AT";
        case LORA_CONFIG_DEFAULT_FAILED: return "AT LOST";
        case LORA_CONFIG_FLASH_FAILED:   return "FLASH FAIL";
        case LORA_CONFIG_ADDR_FAILED:    return "ADDR FAIL";
        case LORA_CONFIG_MODE_FAILED:    return "MODE FAIL";
        case LORA_CONFIG_TMODE_FAILED:   return "TMODE FAIL";
        case LORA_CONFIG_RADIO_FAILED:   return "RADIO FAIL";
        case LORA_CONFIG_POWER_FAILED:   return "POWER FAIL";
        case LORA_CONFIG_SLEEP_FAILED:   return "SLEEP FAIL";
        case LORA_CONFIG_UART_FAILED:    return "UART FAIL";
        default:                         return "UNKNOWN";
    }
}

u8 LoRa_SendPacket(const char *text)
{
    if (!LoRa_WaitIdle(500)) return 0;
    return LoRa_SendRaw(text);
}

u8 LoRa_SendBytes(const u8 *data, u8 length)
{
    if (data == 0 || length == 0) return 0;
    if (!LoRa_WaitIdle(500)) return 0;
    return LoRa_SendBuffer(data, length);
}

u8 LoRa_ReadByte(u8 *data)
{
    if (lora_rx_tail != lora_rx_head)
    {
        *data = lora_rx_buffer[lora_rx_tail];
        lora_rx_tail = (u8)((lora_rx_tail + 1) & (LORA_RX_BUFFER_SIZE - 1));
        return 1;
    }
    return 0;
}

void USART2_IRQHandler(void)
{
    u8 data;
    u8 next;

    if ((USART2->SR & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE)) != 0)
    {
        data = (u8)USART2->DR;
        (void)data;
        lora_rx_overflow++;
        return;
    }

    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        data = (u8)USART_ReceiveData(USART2);
        next = (u8)((lora_rx_head + 1) & (LORA_RX_BUFFER_SIZE - 1));
        if (next != lora_rx_tail)
        {
            lora_rx_buffer[lora_rx_head] = data;
            lora_rx_head = next;
        }
        else lora_rx_overflow++;
    }
}

u8 LoRa_AuxLevel(void)
{
    return (GPIO_ReadInputDataBit(LORA_AUX_PORT, LORA_AUX_PIN) == Bit_SET);
}

u8 LoRa_MD0Level(void)
{
    return (GPIO_ReadOutputDataBit(LORA_MD0_PORT, LORA_MD0_PIN) == Bit_SET);
}

u32 LoRa_GetDetectedBaud(void)      { return lora_detected_baud; }
u8 LoRa_GetDetectedParity(void)     { return lora_detected_parity; }
u8 LoRa_GetLastResponseLength(void) { return lora_last_response_length; }
const char *LoRa_GetLastResponseText(void) { return lora_last_response_text; }
u16 LoRa_GetBaudRegister(void)      { return USART2->BRR; }
u16 LoRa_GetRxOverflowCount(void)   { return lora_rx_overflow; }
