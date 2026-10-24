#ifndef __LORA_H
#define __LORA_H

#include "stm32f10x.h"

#define LORA_MD0_PORT       GPIOB
#define LORA_MD0_PIN        GPIO_Pin_12
#define LORA_AUX_PORT       GPIOB
#define LORA_AUX_PIN        GPIO_Pin_13
#define LORA_RUN_BAUDRATE   115200

typedef enum
{
    LORA_CONFIG_OK = 0,
    LORA_CONFIG_AUX_TIMEOUT,
    LORA_CONFIG_AT_NOT_FOUND,
    LORA_CONFIG_DEFAULT_FAILED,
    LORA_CONFIG_FLASH_FAILED,
    LORA_CONFIG_ADDR_FAILED,
    LORA_CONFIG_MODE_FAILED,
    LORA_CONFIG_TMODE_FAILED,
    LORA_CONFIG_RADIO_FAILED,
    LORA_CONFIG_POWER_FAILED,
    LORA_CONFIG_SLEEP_FAILED,
    LORA_CONFIG_UART_FAILED
} LoRaConfigResult;

void LoRa_Init(void);
LoRaConfigResult LoRa_Configure(void);
const char *LoRa_ConfigResultText(LoRaConfigResult result);
u8 LoRa_SendPacket(const char *text);
u8 LoRa_SendBytes(const u8 *data, u8 length);
u8 LoRa_ReadByte(u8 *data);
u8 LoRa_AuxLevel(void);
u8 LoRa_MD0Level(void);
u32 LoRa_GetDetectedBaud(void);
u8 LoRa_GetDetectedParity(void);
u8 LoRa_GetLastResponseLength(void);
const char *LoRa_GetLastResponseText(void);
u16 LoRa_GetBaudRegister(void);
u16 LoRa_GetRxOverflowCount(void);

#endif
