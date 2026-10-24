#include "EnvFrame.h"

static u8 decoder_state;
static u8 decoder_version;
static u8 decoder_type;
static u16 decoder_sequence;
static u8 decoder_length;
static u8 decoder_index;
static u8 decoder_buffer[16];

u16 EnvFrame_Crc16(const u8 *data, u8 length)
{
    u16 crc;
    u8 index;
    u8 bit;

    crc = 0xFFFF;
    for (index = 0; index < length; index++)
    {
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++)
        {
            if (crc & 1) crc = (u16)((crc >> 1) ^ 0xA001);
            else crc >>= 1;
        }
    }
    return crc;
}

void EnvFrame_DecoderReset(void)
{
    decoder_state = 0;
    decoder_version = 0;
    decoder_type = 0;
    decoder_sequence = 0;
    decoder_length = 0;
    decoder_index = 0;
}

static void EnvFrame_DecoderStart(void)
{
    decoder_state = 1;
    decoder_index = 0;
    decoder_length = 0;
}

u8 EnvFrame_DecoderInput(u8 data, u16 *sequence, EnvSample *sample)
{
    u16 received_crc;
    u16 calculated_crc;

    if (decoder_state == 0)
    {
        if (data == ENV_FRAME_SOF) EnvFrame_DecoderStart();
        return 0;
    }
    if (decoder_state == 1)
    {
        decoder_version = data;
        decoder_buffer[0] = data;
        decoder_state = 2;
        return 0;
    }
    if (decoder_state == 2)
    {
        decoder_type = data;
        decoder_buffer[1] = data;
        decoder_state = 3;
        return 0;
    }
    if (decoder_state == 3)
    {
        decoder_sequence = data;
        decoder_buffer[2] = data;
        decoder_state = 4;
        return 0;
    }
    if (decoder_state == 4)
    {
        decoder_sequence = (u16)(decoder_sequence | ((u16)data << 8));
        decoder_buffer[3] = data;
        decoder_state = 5;
        return 0;
    }
    if (decoder_state == 5)
    {
        decoder_length = data;
        decoder_buffer[4] = data;
        decoder_index = 0;
        if (decoder_length != ENV_FRAME_PAYLOAD_SIZE)
        {
            EnvFrame_DecoderReset();
            if (data == ENV_FRAME_SOF) EnvFrame_DecoderStart();
            return 0;
        }
        decoder_state = 6;
        return 0;
    }
    if (decoder_state == 6)
    {
        decoder_buffer[5 + decoder_index] = data;
        decoder_index++;
        if (decoder_index >= decoder_length)
        {
            decoder_state = 7;
            decoder_index = 0;
        }
        return 0;
    }
    if (decoder_state == 7)
    {
        decoder_buffer[5 + decoder_length + decoder_index] = data;
        decoder_index++;
        if (decoder_index < 2) return 0;
        received_crc = (u16)decoder_buffer[5 + decoder_length];
        received_crc = (u16)(received_crc |
                             ((u16)decoder_buffer[6 + decoder_length] << 8));
        calculated_crc = EnvFrame_Crc16(decoder_buffer,
                                        (u8)(5 + decoder_length));
        if (decoder_version != ENV_FRAME_VERSION ||
            decoder_type != ENV_FRAME_TYPE_ENV ||
            received_crc != calculated_crc)
        {
            EnvFrame_DecoderReset();
            return 0;
        }
        if (sequence != 0) *sequence = decoder_sequence;
        if (sample != 0)
        {
            sample->temperature_x10 = (s16)((u16)decoder_buffer[5] |
                                             ((u16)decoder_buffer[6] << 8));
            sample->humidity_x10 = (u16)decoder_buffer[7] |
                                   ((u16)decoder_buffer[8] << 8);
            sample->light_lux = (u16)decoder_buffer[9] |
                                ((u16)decoder_buffer[10] << 8);
            sample->battery_mv = (u16)decoder_buffer[11] |
                                 ((u16)decoder_buffer[12] << 8);
            sample->status = decoder_buffer[13];
        }
        EnvFrame_DecoderReset();
        return 1;
    }
    EnvFrame_DecoderReset();
    return 0;
}

u8 EnvFrame_Encode(u16 sequence, const EnvSample *sample,
                   u8 *frame, u8 frame_size)
{
    u16 crc;

    if (sample == 0 || frame == 0 || frame_size < 17) return 0;
    frame[0] = ENV_FRAME_SOF;
    frame[1] = ENV_FRAME_VERSION;
    frame[2] = ENV_FRAME_TYPE_ENV;
    frame[3] = (u8)sequence;
    frame[4] = (u8)(sequence >> 8);
    frame[5] = ENV_FRAME_PAYLOAD_SIZE;
    frame[6] = (u8)sample->temperature_x10;
    frame[7] = (u8)((u16)sample->temperature_x10 >> 8);
    frame[8] = (u8)sample->humidity_x10;
    frame[9] = (u8)(sample->humidity_x10 >> 8);
    frame[10] = (u8)sample->light_lux;
    frame[11] = (u8)(sample->light_lux >> 8);
    frame[12] = (u8)sample->battery_mv;
    frame[13] = (u8)(sample->battery_mv >> 8);
    frame[14] = sample->status;
    crc = EnvFrame_Crc16(&frame[1], 14);
    frame[15] = (u8)crc;
    frame[16] = (u8)(crc >> 8);
    return 17;
}
