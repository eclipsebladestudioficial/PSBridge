#include <delaythread.h>
#include <stdint.h>
#include <string.h>

#include <psbridge.h>
#include "psbridge_log.h"

#define LIB_AUDIO 0xf41677cc
#define LIB_SAS 0x1d46fbf4
#define LIB_ATRAC 0x75263ea8
#define ATRAC_LIMIT 4
#define ATRAC3_SAMPLES 1024
#define ATRAC3PLUS_SAMPLES 2048
#define ATRAC3_CODEC 0x1001
#define ATRAC3PLUS_CODEC 0x1000

typedef struct {
    int used;
    int initialized;
    u32 codec;
    u32 source_size;
    u32 output_channels;
    u32 buffer_address;
    u32 buffer_size;
    u32 block_align;
    u32 bytes_per_second;
    u32 sample_count;
    u32 position;
    u32 loop_start;
    u32 loop_end;
    int loop_count;
    int has_loop;
} AtracContext;

typedef struct {
    int initialized;
    u32 core;
    u32 grain;
    u32 output_mode;
} SasContext;

typedef struct {
    int reserved;
    u32 samples;
    u32 format;
} PspAudioChannel;

static PspAudioChannel audio_channels[8];

static AtracContext atrac_ids[ATRAC_LIMIT];
static SasContext sas;
static u32 audio2_samples = 1024;
static int audio2_reserved;
static unsigned audio_diag_calls;
static unsigned atrac_diag_calls;


static void *audio_pointer(u32 address, u32 size)
{
    u32 offset;

    if (!address || !size || address + size < address)
        return NULL;

    if (address >= guest_info.base && address - guest_info.base < guest_info.memory_size) {
        offset = address - guest_info.base;
        if (size > guest_info.memory_size - offset)
            return NULL;
    } else if (address >= 0x80001000u && address < 0x82000000u &&
               size <= 0x82000000u - address) {
    } else if (address >= 0xa0001000u && address < 0xa2000000u &&
               size <= 0xa2000000u - address) {
    } else if (address < 0x1000 || address >= 0x02000000 ||
               size > 0x02000000 - address) {
        return NULL;
    }

    return psbridge_game_pointer((const void *)(uintptr_t)address);
}

static u16 atrac_u16(const u8 *data)
{
    return (u16)data[0] | ((u16)data[1] << 8);
}

static u32 atrac_u32(const u8 *data)
{
    return (u32)data[0] | ((u32)data[1] << 8) |
        ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static int atrac_load(AtracContext *id, u32 address, u32 size)
{
    const u8 *wave;
    u32 position;
    int format_found = 0;
    int data_found = 0;

    if (size < 44 || size > 16u * 1024u * 1024u)
        return -1;

    wave = audio_pointer(address, size);
    if (!wave || memcmp(wave, "RIFF", 4) || memcmp(wave + 8, "WAVE", 4))
        return -1;

    id->buffer_address = address;
    id->buffer_size = size;
    id->position = 0;
    id->sample_count = 0;
    id->block_align = 0;
    id->bytes_per_second = 0;
    id->loop_count = 0;
    id->has_loop = 0;
    id->initialized = 0;

    for (position = 12; position <= size - 8;) {
        const u8 *chunk = wave + position;
        u32 chunk_size = atrac_u32(chunk + 4);
        u32 remaining = size - position - 8;
        u32 advance;

        if (chunk_size > remaining)
            return -1;

        if (!memcmp(chunk, "fmt ", 4) && chunk_size >= 16) {
            u16 format = atrac_u16(chunk + 8);
            u16 channels = atrac_u16(chunk + 10);
            u16 block_align = atrac_u16(chunk + 20);

            if ((format != 0xfffe && format != 0x0270) ||
                (channels != 1 && channels != 2) || block_align == 0)
                return -1;

            id->output_channels = channels;
            id->block_align = block_align;
            id->bytes_per_second = atrac_u32(chunk + 16);
            id->codec = format == 0x0270 ? ATRAC3_CODEC : ATRAC3PLUS_CODEC;
            format_found = 1;
        } else if (!memcmp(chunk, "fact", 4) && chunk_size >= 4) {
            id->sample_count = atrac_u32(chunk + 8);
        } else if (!memcmp(chunk, "smpl", 4) && chunk_size >= 60 &&
                   atrac_u32(chunk + 36) != 0) {
            id->loop_start = atrac_u32(chunk + 52);
            id->loop_end = atrac_u32(chunk + 56);
            id->has_loop = 1;
        } else if (!memcmp(chunk, "data", 4) && chunk_size > 0) {
            data_found = 1;
            if (!id->sample_count && id->block_align)
                id->sample_count = (chunk_size / id->block_align) * ATRAC3PLUS_SAMPLES;
        }

        advance = chunk_size + (chunk_size & 1u);
        if (advance > remaining)
            return -1;
        position += 8 + advance;
    }

    if (!format_found || !data_found || !id->sample_count)
        return -1;

    id->initialized = 1;
    return 0;
}

static int atrac_new(u32 address, u32 size)
{
    int index;

    for (index = 0; index < ATRAC_LIMIT; index++) {
        if (!atrac_ids[index].used) {
            AtracContext *id = &atrac_ids[index];

            memset(id, 0, sizeof(*id));
            id->used = 1;
            if (atrac_load(id, address, size) < 0) {
                memset(id, 0, sizeof(*id));
                return -1;
            }

            if (atrac_diag_calls++ < 24)
                PRINT_LOG("atrac botou musica pra gozar id %u tamanho %u canais %u amostras %u loop %u\n",
                    index, size, id->output_channels, id->sample_count, id->has_loop);
            return index;
        }
    }

    return -1;
}

static u64 atrac_call(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4)
{
    AtracContext *id;
    u32 *consumed;
    u32 *produced;
    void *pcm;
    u32 pcm_bytes;
    int index;

    if (nid == 0x132f1eca)
        return 0;

    if (nid == 0x780f88d1) {
        if (a0 != ATRAC3_CODEC && a0 != ATRAC3PLUS_CODEC)
            return (u32)-1;

        for (index = 0; index < ATRAC_LIMIT; index++) {
            if (!atrac_ids[index].used) {
                memset(&atrac_ids[index], 0, sizeof(atrac_ids[index]));
                atrac_ids[index].used = 1;
                atrac_ids[index].codec = a0;
                return index;
            }
        }

        return (u32)-1;
    }

    if (nid == 0x7a20e7af)
        return (u32)atrac_new(a0, a1);

    if (a0 >= ATRAC_LIMIT || !atrac_ids[a0].used)
        return (u32)-1;

    id = &atrac_ids[a0];

    if (nid == 0x868120b5) {
        if (!id->initialized || !id->has_loop)
            return (u32)-1;
        id->loop_count = (int)a1;
        return 0;
    }

    if (nid == 0x0e2a73ab)
        return (u32)atrac_load(id, a1, a2);

    if (nid == 0xfaa4f89b) {
        u32 *loops = audio_pointer(a1, sizeof(u32));
        u32 *has_loop = audio_pointer(a2, sizeof(u32));
        if (!loops || !has_loop || !id->initialized)
            return (u32)-1;
        *loops = (u32)id->loop_count;
        *has_loop = id->has_loop;
        return 0;
    }

    if (nid == 0x9ae849a7 || nid == 0x36faabfb ||
        nid == 0xd6a5f2f7 || nid == 0xe23e3a35 ||
        nid == 0xb3b5d042 || nid == 0x31668baa ||
        nid == 0xa554a158) {
        u32 *value = audio_pointer(a1, sizeof(u32));

        if (!value || !id->initialized)
            return (u32)-1;

        if (nid == 0x9ae849a7)
            *value = 0xffffffffu;
        else if (nid == 0x36faabfb)
            *value = id->position >= id->sample_count ? 0 :
                id->sample_count - id->position > (id->codec == ATRAC3_CODEC ? ATRAC3_SAMPLES : ATRAC3PLUS_SAMPLES) ?
                (id->codec == ATRAC3_CODEC ? ATRAC3_SAMPLES : ATRAC3PLUS_SAMPLES) : id->sample_count - id->position;
        else if (nid == 0xd6a5f2f7)
            *value = id->codec == ATRAC3_CODEC ? ATRAC3_SAMPLES : ATRAC3PLUS_SAMPLES;
        else if (nid == 0xe23e3a35)
            *value = id->position;
        else if (nid == 0xb3b5d042)
            *value = id->output_channels;
        else if (nid == 0x31668baa)
            *value = id->output_channels;
        else if (nid == 0xa554a158)
            *value = (u32)(((u64)id->bytes_per_second * 8 + 500) / 1000);
        else
            *value = 0;
        return 0;
    }

    if (nid == 0xa2bba8be) {
        u32 *total = audio_pointer(a1, sizeof(u32));
        u32 *loop_start = a2 ? audio_pointer(a2, sizeof(u32)) : NULL;
        u32 *loop_end = a3 ? audio_pointer(a3, sizeof(u32)) : NULL;

        if (!id->initialized || !total || (a2 && !loop_start) ||
            (a3 && !loop_end))
            return (u32)-1;
        *total = id->sample_count;
        if (loop_start)
            *loop_start = id->has_loop ? id->loop_start : 0xffffffffu;
        if (loop_end)
            *loop_end = id->has_loop ? id->loop_end : 0xffffffffu;
        return 0;
    }

    if (nid == 0x6a8c3cd5) {
        u32 count;
        u32 bytes;
        u32 *samples = audio_pointer(a2, sizeof(u32));
        u32 *finished = audio_pointer(a3, sizeof(u32));
        u32 *remaining = audio_pointer(a4, sizeof(u32));
        void *output;

        if (!id->initialized || !samples || !finished || !remaining)
            return (u32)-1;

        if (id->position >= id->sample_count && id->has_loop &&
            id->loop_count != 0) {
            id->position = id->loop_start;
            if (id->loop_count > 0)
                id->loop_count--;
        }

        count = id->position >= id->sample_count ? 0 :
            id->sample_count - id->position;
        if (count > (id->codec == ATRAC3_CODEC ? ATRAC3_SAMPLES : ATRAC3PLUS_SAMPLES))
            count = id->codec == ATRAC3_CODEC ? ATRAC3_SAMPLES : ATRAC3PLUS_SAMPLES;

        bytes = count * id->output_channels * sizeof(int16_t);
        output = bytes ? audio_pointer(a1, bytes) : NULL;
        if (bytes && !output)
            return (u32)-1;

        if (bytes)
            memset(output, 0, bytes);
        id->position += count;
        *samples = count;
        *finished = id->position >= id->sample_count &&
            (!id->has_loop || id->loop_count == 0);
        *remaining = 0xffffffffu;
        DelayThread(1000);
        return 0;
    }

    if (nid == 0x61eb33f5) {
        memset(id, 0, sizeof(*id));
        return 0;
    }

    if (nid == 0x1575d64b) {
        u32 *params = audio_pointer(a1, 12);

        if (!params)
            return (u32)-1;

        id->output_channels = params[1];
        id->source_size = params[2];

        if ((id->output_channels != 1 && id->output_channels != 2) ||
            !id->source_size || id->source_size > 8192)
            return (u32)-1;

        id->initialized = 1;
        if (atrac_diag_calls++ < 24)
            PRINT_LOG("atrac acordou id %u codec %08x canais %u pacote %u\n", a0, id->codec, id->output_channels, id->source_size);
        return 0;
    }

    if (nid != 0x0c116e1b || !id->initialized)
        return (u32)-1;

    pcm_bytes = (
        id->codec == ATRAC3_CODEC ?
        ATRAC3_SAMPLES :
        ATRAC3PLUS_SAMPLES
    ) * id->output_channels * sizeof(int16_t);
    consumed = audio_pointer(a2, sizeof(u32));
    pcm = audio_pointer(a3, pcm_bytes);
    produced = audio_pointer(a4, sizeof(u32));

    if (!consumed || !pcm || !produced || !audio_pointer(a1, id->source_size))
        return (u32)-1;

    memset(pcm, 0, pcm_bytes);
    *consumed = id->source_size;
    *produced = pcm_bytes;
    if (atrac_diag_calls++ < 24)
        PRINT_LOG("atrac gozou silencio id %u comeu %u entregou %u\n", a0, *consumed, *produced);
    DelayThread(1000);
    return 0;
}

static int audio_valid_length(u32 samples)
{
    return samples >= 64 && samples <= 65472 && !(samples & 63u);
}

static u64 audio2_call(u32 nid, u32 a0)
{
    if (nid == 0x01562ba3) {
        if (a0 >= 17 && a0 <= 4111)
            audio2_samples = a0;
        else
            return (u32)-1;
        audio2_reserved = 1;
        return 0;
    }

    if (nid == 0x63f2889c) {
        if (!audio2_reserved || a0 < 17 || a0 > 4111)
            return (u32)-1;
        audio2_samples = a0;
        return 0;
    }

    if (nid == 0x43196845) {
        audio2_reserved = 0;
        return 0;
    }

    if (nid == 0x647cef33)
        return 0;

    if (nid == 0x2d53f36e) {
        u32 wait;

        if (!audio2_reserved)
            return (u32)-1;

        wait = (u32)(((u64)audio2_samples * 1000000u + 44099u) / 44100u);
        DelayThread(wait);
        return 0;
    }

    return (u32)-1;
}

static u64 audio_channel_call(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3)
{
    PspAudioChannel *channel;
    int id;

    if (nid == 0x01562ba3 || nid == 0x63f2889c ||
        nid == 0x43196845 || nid == 0x647cef33 ||
        nid == 0x2d53f36e)
        return audio2_call(nid, a0);

    if (nid == 0x5ec81c55) {
        if (!audio_valid_length(a1) || (a2 != 0 && a2 != 0x10))
            return (u32)-1;

        if (a0 == 0xffffffffu) {
            for (id = 0; id < 8; id++) {
                if (!audio_channels[id].reserved)
                    break;
            }
        } else {
            id = (int)a0;
        }

        if (id < 0 || id >= 8 || audio_channels[id].reserved)
            return (u32)-1;

        audio_channels[id].reserved = 1;
        audio_channels[id].samples = a1;
        audio_channels[id].format = a2;
        PRINT_LOG("audio reservou a frica canal %d amostras %u formato %u\n", id, a1, a2);
        return (u32)id;
    }

    if (a0 >= 8 || !audio_channels[a0].reserved)
        return (u32)-1;

    channel = &audio_channels[a0];

    if (nid == 0x6fc46853) {
        memset(channel, 0, sizeof(*channel));
        return 0;
    }

    if (nid == 0xcb2e439e) {
        if (!audio_valid_length(a1))
            return (u32)-1;
        channel->samples = a1;
        return 0;
    }

    if (nid == 0x95fd0c2d) {
        if (a1 != 0 && a1 != 0x10)
            return (u32)-1;
        channel->format = a1;
        return 0;
    }

    if (nid == 0xb011922f || nid == 0xe9d97901)
        return 0;

    if (nid == 0xb7e1d8e7)
        return 0;

    if (nid == 0x136caf51 || nid == 0x13f592bc ||
        nid == 0x8c1009b2 || nid == 0xe2d56b2d) {
        u32 address = (nid == 0x13f592bc || nid == 0xe2d56b2d) ? a3 : a2;
        u32 channels = channel->format == 0x10 ? 1 : 2;
        u32 bytes = channel->samples * channels * sizeof(int16_t);
        static u32 output_logs;

        if (address && !audio_pointer(address, bytes))
            return (u32)-1;

        if (nid == 0x136caf51 || nid == 0x13f592bc) {
            u32 delay = (u32)(((u64)channel->samples * 1000000u + 44099u) / 44100u);
            if (output_logs++ < 12)
                PRINT_LOG("audio segurou a putaria canal %u amostras %u espera %u\n", a0, channel->samples, delay);
            DelayThread(delay);
            return channel->samples;
        }

        return 0;
    }

    return (u32)-1;
}

static int valid_sas_grain(u32 grain)
{
    return grain >= 0x40 && grain <= 0x800 && !(grain & 0x1f);
}

static u64 sas_mix(u32 address)
{
    void *buffer;
    u32 channels;
    u32 bytes;

    if (!sas.initialized)
        return (u32)-1;

    channels = sas.output_mode ? 1 : 2;
    bytes = sas.grain * channels * sizeof(int16_t);
    buffer = audio_pointer(address, bytes);

    if (!buffer)
        return (u32)-1;

    memset(buffer, 0, bytes);
    return 0;
}

static u64 sas_call(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4)
{
    if (nid == 0x42778a9f) {
        if ((a0 & 0x3f) || !audio_pointer(a0, sizeof(u32)) ||
            !valid_sas_grain(a1) || !a2 || a2 > 32 || a3 > 1 || a4 != 44100)
            return (u32)-1;

        sas.initialized = 1;
        sas.core = a0;
        sas.grain = a1;
        sas.output_mode = a3;
        return 0;
    }

    if (nid == 0xa3589d81 || nid == 0x50a14dfc)
        return sas_mix(a1);

    if (nid == 0xbd11b7c2)
        return sas.initialized ? sas.grain : (u32)-1;

    if (nid == 0xd1e0a01e) {
        if (!sas.initialized || !valid_sas_grain(a1))
            return (u32)-1;

        sas.grain = a1;
        return 0;
    }

    if (nid == 0xe175ef66)
        return sas.initialized ? sas.output_mode : (u32)-1;

    if (nid == 0xe855bf76) {
        if (!sas.initialized || a1 > 1)
            return (u32)-1;

        sas.output_mode = a1;
        return 0;
    }

    if (nid == 0x07f58c24) {
        u32 *heights = audio_pointer(a1, 32 * sizeof(u32));

        if (!heights)
            return (u32)-1;

        memset(heights, 0, 32 * sizeof(*heights));
        return 0;
    }

    if (nid == 0x68a46b95)
        return 0xffffffff;

    return 0;
}

u64 psbridge_audio(u32 library, u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4)
{
    u64 result;

    if (library == LIB_AUDIO)
        result = audio_channel_call(nid, a0, a1, a2, a3);
    else if (library == LIB_SAS)
        result = sas_call(nid, a0, a1, a2, a3, a4);
    else if (library == LIB_ATRAC)
        result = atrac_call(nid, a0, a1, a2, a3, a4);
    else
        result = (u32)-1;

    if (audio_diag_calls < 32 &&
        ((u32)result == 0xffffffffu || nid == 0x01562ba3 || nid == 0x43196845 || nid == 0x780f88d1)) {
        audio_diag_calls++;
        PRINT_LOG("audio chamou a biblioteca %08x nid %08x pegou %08x pediu %08x %08x retornou %08x\n",
            library, nid, a0, a1, a2, (u32)result);
    }

    return result;
}
