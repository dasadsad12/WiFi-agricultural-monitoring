#ifndef __ENV_FRAME_H
#define __ENV_FRAME_H

#include "stm32f10x.h"

#define ENV_FRAME_SOF          0xA5
#define ENV_FRAME_VERSION      0x02
#define ENV_FRAME_TYPE_ENV     0x01
#define ENV_FRAME_PAYLOAD_SIZE 9
#define ENV_FRAME_MAX_SIZE     32

typedef struct
{
    s16 temperature_x10;
    u16 humidity_x10;
    u16 light_lux;
    u16 battery_mv;
    u8 status;
} EnvSample;

void EnvFrame_DecoderReset(void);
u8 EnvFrame_DecoderInput(u8 data, u16 *sequence, EnvSample *sample);
u8 EnvFrame_Encode(u16 sequence, const EnvSample *sample,
                   u8 *frame, u8 frame_size);
u16 EnvFrame_Crc16(const u8 *data, u8 length);

#endif
