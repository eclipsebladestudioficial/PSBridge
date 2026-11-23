#include <fcntl.h>
#include <kernel.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>
#include <jerror.h>

#include "psbridge.h"

#define MPEG_STREAM_LIMIT 32
#define MPEG_PACKET_BYTES 2048
#define MPEG_PCM_BYTES 8192
#define MPEG_INVALID_VALUE 0x806101feu
#define MPEG_NO_DATA 0x80618001u

typedef struct {
    u32 pts_low;
    u32 pts_high;
    u32 packet;
} MpegTimestamp;

typedef struct {
    u32 type;
    u32 channel;
    u32 mode;
    int registered;
    int file;
    u32 au_address;
    u32 pending;
    u32 width;
    u32 height;
    u32 frames;
    u32 consumed;
    int ended;
    u64 audio_pts;
    u32 audio_bytes;
    MpegTimestamp timestamp;
} MpegStream;

typedef struct {
    char signature[12];
    u32 reserved;
    u32 ring_address;
    u32 es_buffers;
    MpegStream streams[MPEG_STREAM_LIMIT];
    char header[17];
    char key[34];
    u32 packets;
    u32 stride;
    u32 pixel_format;
    u32 result;
    u32 frames;
    u32 width;
    u32 height;
    u32 frame_ready;
} MpegContext;

typedef struct {
    MpegStream *stream;
    int file;
    u32 decoded;
    struct jpeg_decompress_struct jpeg;
    struct jpeg_error_mgr error;
    struct jpeg_source_mgr source;
    jmp_buf failure;
} MpegDecoder;

static MpegDecoder decoder;
static int decoder_lock = -1;
static u8 input_buffer[16384];
static u32 picture_row[480];

static MpegContext *mpeg_context(u32 address)
{
    const u32 *handle = psbridge_guest_buffer(address, sizeof(*handle));
    MpegContext *context = handle ? psbridge_guest_buffer(*handle, sizeof(*context)) : NULL;
    return context && !memcmp(context->signature, "LIBMPEG", 8) ? context : NULL;
}

static u32 big_endian_word(const u8 *bytes)
{
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) | ((u32)bytes[2] << 8) | bytes[3];
}

static void mpeg_key(char key[17], const u8 *block)
{
    u64 hash = 14695981039346656037ull;
    unsigned i;

    for (i = 0; i < MPEG_PACKET_BYTES; ++i)
        hash = (hash ^ block[i]) * 1099511628211ull;
    snprintf(key, 17, "%08lx%08lx", (unsigned long)(hash >> 32), (unsigned long)(u32)hash);
}

static int read_block(int file, void *output, u32 size)
{
    u8 *bytes = output;
    u32 total = 0;

    while (total < size) {
        int count = read(file, bytes + total, size - total);
        if (count < 0)
            return -1;
        if (count == 0)
            break;
        total += count;
    }
    return total;
}

static int open_track(const MpegContext *context, char type, u32 channel, const char *extension)
{
    char path[84];

    if (!context->key[0] || channel > 255)
        return -1;

    snprintf(path, sizeof(path), "host:converted/.video/%s/%c%02lx.%s",
        context->key, type, (unsigned long)channel, extension);
    return open(path, O_RDONLY);
}

static void find_movie(MpegContext *context, const u8 *packet)
{
    char key[17];
    char path[52];
    int file;

    mpeg_key(key, packet);
    if (context->header[0]) {
        snprintf(context->key, sizeof(context->key), "%s-%s", context->header, key);
        return;
    }
    snprintf(path, sizeof(path), "host:converted/.video/p%s", key);
    file = open(path, O_RDONLY);
    if (file >= 0) {
        if (read_block(file, context->key, 33) != 33)
            context->key[0] = 0;
        close(file);
    }
}

static void close_decoder(void)
{
    jpeg_destroy_decompress(&decoder.jpeg);
    if (decoder.stream)
        close(decoder.file);
    decoder.stream = NULL;
    decoder.decoded = 0;
}

static void reset_stream(MpegStream *stream)
{
    if (decoder_lock >= 0) {
        WaitSema(decoder_lock);
        if (decoder.stream == stream)
            close_decoder();
        SignalSema(decoder_lock);
    }
    if (stream->file >= 0)
        close(stream->file);

    *stream = (MpegStream){
        .registered = stream->registered,
        .type = stream->type,
        .channel = stream->channel,
        .mode = stream->mode,
        .file = -1,
    };
}

static void update_ring(MpegContext *context)
{
    u32 *ring = psbridge_guest_buffer(context->ring_address, 44);
    if (!ring)
        return;

    u32 consumed = context->packets;
    unsigned i;
    for (i = 0; i < MPEG_STREAM_LIMIT; ++i) {
        MpegStream *stream = &context->streams[i];
        if (stream->registered && stream->type == 0 && !stream->mode && !stream->ended &&
            stream->consumed < consumed)
            consumed = stream->consumed;
    }
    ring[1] = context->packets;
    ring[2] = context->packets;
    ring[3] = context->packets - consumed;
}

static void reset_streams(MpegContext *context)
{
    unsigned i;

    for (i = 0; i < MPEG_STREAM_LIMIT; ++i)
        if (context->streams[i].registered)
            reset_stream(&context->streams[i]);
    context->packets = 0;
    update_ring(context);
}

static void decoder_error(j_common_ptr jpeg)
{
    (void)jpeg;
    longjmp(decoder.failure, 1);
}

static void decoder_warning(j_common_ptr jpeg, int level)
{
    if (level < 0)
        decoder_error(jpeg);
}

static void source_marker(j_decompress_ptr jpeg)
{
    (void)jpeg;
}

static boolean fill_picture_input(j_decompress_ptr jpeg)
{
    int bytes = read(decoder.file, input_buffer, sizeof(input_buffer));
    if (bytes <= 0)
        ERREXIT(jpeg, JERR_INPUT_EOF);
    jpeg->src->next_input_byte = input_buffer;
    jpeg->src->bytes_in_buffer = bytes;
    return TRUE;
}

static void skip_picture_input(j_decompress_ptr jpeg, long bytes)
{
    if (bytes <= 0)
        return;
    while ((unsigned long)bytes > jpeg->src->bytes_in_buffer) {
        bytes -= jpeg->src->bytes_in_buffer;
        fill_picture_input(jpeg);
    }
    jpeg->src->next_input_byte += bytes;
    jpeg->src->bytes_in_buffer -= bytes;
}

static int open_decoder(const MpegContext *context, MpegStream *stream)
{
    close_decoder();
    int file = open_track(context, 'v', stream->channel, "mjpg");
    if (file < 0)
        return 0;

    decoder.stream = stream;
    decoder.file = file;
    decoder.jpeg.err = jpeg_std_error(&decoder.error);
    decoder.error.error_exit = decoder_error;
    decoder.error.emit_message = decoder_warning;
    jpeg_create_decompress(&decoder.jpeg);
    decoder.source = (struct jpeg_source_mgr){
        .init_source = source_marker,
        .fill_input_buffer = fill_picture_input,
        .skip_input_data = skip_picture_input,
        .resync_to_restart = jpeg_resync_to_restart,
        .term_source = source_marker,
    };
    decoder.jpeg.src = &decoder.source;
    return 1;
}

static u16 video_pixel16(u32 pixel, u32 format)
{
    if (format == 0)
        return ((pixel >> 3) & 0x001f) | ((pixel >> 5) & 0x07e0) | ((pixel >> 8) & 0xf800);
    if (format == 1)
        return ((pixel >> 3) & 0x001f) | ((pixel >> 6) & 0x03e0) | ((pixel >> 9) & 0x7c00) | 0x8000;
    return ((pixel >> 4) & 0x000f) | ((pixel >> 8) & 0x00f0) | ((pixel >> 12) & 0x0f00) | 0xf000;
}

static int decode_picture(const MpegContext *context, MpegStream *stream, void *output, u32 stride)
{
    u32 y, x;

    if (setjmp(decoder.failure)) {
        close_decoder();
        return 0;
    }
    if (decoder.stream != stream && !open_decoder(context, stream))
        return 0;

    do {
        struct jpeg_decompress_struct *jpeg = &decoder.jpeg;
        if (jpeg_read_header(jpeg, TRUE) != JPEG_HEADER_OK ||
            jpeg->image_width != stream->width || jpeg->image_height != stream->height ||
            jpeg_has_multiple_scans(jpeg)) {
            close_decoder();
            return 0;
        }
        jpeg->out_color_space = JCS_EXT_RGBA;
        jpeg->dct_method = JDCT_IFAST;
        jpeg->do_fancy_upsampling = FALSE;
        jpeg_start_decompress(jpeg);
        for (y = 0; y < stream->height; ++y) {
            JSAMPROW row = (JSAMPROW)picture_row;
            if (decoder.decoded == stream->frames && context->pixel_format == 3)
                row = (JSAMPROW)((u32 *)output + y * stride);
            jpeg_read_scanlines(jpeg, &row, 1);
            if (decoder.decoded == stream->frames && context->pixel_format != 3) {
                u16 *pixels = (u16 *)output + y * stride;
                for (x = 0; x < stream->width; ++x)
                    pixels[x] = video_pixel16(picture_row[x], context->pixel_format);
            }
        }
        jpeg_finish_decompress(jpeg);
        ++decoder.decoded;
    } while (decoder.decoded <= stream->frames);

    psbridge_graphics_draw_invalidate();
    return 1;
}

static MpegStream *registered_stream(MpegContext *context, u32 id)
{
    if (!context || !id || id > MPEG_STREAM_LIMIT || !context->streams[id - 1].registered)
        return NULL;
    return &context->streams[id - 1];
}

static MpegStream *pending_stream(MpegContext *context, u32 au_address, int audio)
{
    unsigned i;
    for (i = 0; i < MPEG_STREAM_LIMIT; ++i) {
        MpegStream *stream = &context->streams[i];
        if (stream->registered && (stream->type != 0) == audio && stream->pending &&
            stream->au_address == au_address)
            return stream;
    }
    return NULL;
}

static int open_stream(MpegContext *context, MpegStream *stream, int audio)
{
    if (stream->file >= 0)
        return 1;
    stream->file = open_track(context, audio ? 'a' : 'v', stream->channel, audio ? "pcm" : "pts");
    if (stream->file < 0)
        return 0;
    if (audio) {
        off_t end = lseek(stream->file, 0, SEEK_END);
        if (end > 8 && lseek(stream->file, 0, SEEK_SET) == 0 &&
            read_block(stream->file, &stream->audio_pts, 8) == 8) {
            stream->audio_bytes = end - 8;
            return 1;
        }
    } else {
        u32 dimensions[2];
        if (read_block(stream->file, dimensions, sizeof(dimensions)) == sizeof(dimensions) &&
            dimensions[0] > 0 && dimensions[0] <= 480 && dimensions[1] > 0 && dimensions[1] <= 272) {
            stream->width = dimensions[0];
            stream->height = dimensions[1];
            return 1;
        }
    }
    close(stream->file);
    stream->file = -1;
    return 0;
}

static u32 get_access_unit(MpegContext *context, MpegStream *stream, u32 address, u32 attr_address, int audio)
{
    u32 *au = psbridge_guest_buffer(address, 24);
    u32 *attr = psbridge_guest_buffer(attr_address, 4);
    u64 pts;

    if (!stream || !au || (audio ? (stream->type != 1 && stream->type != 15) : stream->type != 0))
        return MPEG_INVALID_VALUE;
    if (stream->ended || !context->packets)
        return MPEG_NO_DATA;
    if (!open_stream(context, stream, audio)) {
        stream->ended = 1;
        update_ring(context);
        return MPEG_NO_DATA;
    }
    if (audio) {
        if (!stream->audio_bytes) {
            au[2] = au[3] = (u32)-1;
            return MPEG_NO_DATA;
        }
        pts = stream->audio_pts + ((u64)stream->frames * 2048 * 90000) / 44100;
    } else {
        if (!stream->timestamp.packet &&
            read_block(stream->file, &stream->timestamp, sizeof(stream->timestamp)) != sizeof(stream->timestamp)) {
            stream->ended = 1;
            update_ring(context);
            au[2] = au[3] = (u32)-1;
            return MPEG_NO_DATA;
        }
        if (context->packets < stream->timestamp.packet)
            return MPEG_NO_DATA;
        pts = ((u64)stream->timestamp.pts_high << 32) | stream->timestamp.pts_low;
    }
    au[0] = pts >> 32;
    au[1] = (u32)pts;
    u64 dts = audio ? pts : pts - 3003;
    au[2] = dts >> 32;
    au[3] = (u32)dts;
    au[5] = audio ? 2112 : MPEG_PACKET_BYTES;
    if (attr)
        *attr = audio ? 0 : 1;
    stream->au_address = address;
    stream->pending = 1;
    return 0;
}

static u32 decode_audio(MpegContext *context, u32 au_address, u32 output)
{
    u8 *pcm = psbridge_guest_buffer(output, MPEG_PCM_BYTES);
    if (!context || !pcm)
        return MPEG_INVALID_VALUE;
    MpegStream *stream = pending_stream(context, au_address, 1);
    if (!stream)
        return MPEG_NO_DATA;
    int bytes = read_block(stream->file, pcm, MPEG_PCM_BYTES);
    stream->pending = 0;
    if (bytes <= 0) {
        stream->audio_bytes = 0;
        return MPEG_NO_DATA;
    }
    stream->audio_bytes -= bytes;
    memset(pcm + bytes, 0, MPEG_PCM_BYTES - bytes);
    ++stream->frames;
    return 0;
}

static u32 decode_video(MpegContext *context, u32 au_address, u32 stride, u32 buffer_address, u32 status_address)
{
    const u32 *buffer = psbridge_guest_buffer(buffer_address, 4);
    u32 *status = psbridge_guest_buffer(status_address, 4);
    if (!context || !buffer || !status || decoder_lock < 0)
        return MPEG_INVALID_VALUE;
    *status = 0;
    context->frame_ready = 0;

    MpegStream *stream = pending_stream(context, au_address, 0);
    if (!stream)
        return MPEG_NO_DATA;
    if (!stride)
        stride = context->stride;
    if (stride < stream->width || stride > 2048)
        return MPEG_INVALID_VALUE;
    u32 pixels = (stream->height - 1) * stride + stream->width;
    void *output = psbridge_guest_buffer(*buffer, pixels * (context->pixel_format == 3 ? 4 : 2));
    if (!output)
        return MPEG_INVALID_VALUE;

    WaitSema(decoder_lock);
    if (decode_picture(context, stream, output, stride)) {
        ++stream->frames;
        ++context->frames;
        context->width = stream->width;
        context->height = stream->height;
        context->frame_ready = 1;
        *status = 1;
    }
    SignalSema(decoder_lock);
    stream->pending = 0;
    stream->consumed = stream->timestamp.packet - 1;
    stream->timestamp.packet = 0;
    update_ring(context);
    context->result = *status ? 0 : MPEG_NO_DATA;
    return context->result;
}

extern inline __attribute__((always_inline)) u64 psbridge_video_mpeg(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    int i;

    switch (nid) {
        case 0x37295ed8: {
            u32 *ring = psbridge_game_pointer((void *)(uintptr_t)a0);
            if (!ring || a1 > 8192u || a1 * 2152u > a3)
                return (u32)-1;
            memset(ring, 0, 48);
            ring[0] = a1;
            ring[4] = 2048;
            ring[5] = a2;
            ring[6] = a4;
            ring[7] = a5;
            ring[8] = a2 + a1 * 2048u;
            return 0;
        }
        case 0x682a619b: {
            ee_sema_t sema = { .init_count = 1, .max_count = 1 };
            if (decoder_lock < 0)
                decoder_lock = CreateSema(&sema);
            return decoder_lock < 0 ? MPEG_INVALID_VALUE : 0;
        }
        case 0x874624d6:
        case 0x13407f13:
            return 0;
        case 0x707b7629:
        case 0x500f0429:
        case 0x4571cc64: {
            MpegContext *context = mpeg_context(a0);
            if (!context)
                return MPEG_INVALID_VALUE;
            if (nid == 0x707b7629)
                reset_streams(context);
            else if (nid == 0x500f0429) {
                MpegStream *stream = registered_stream(context, a1);
                if (!stream)
                    return MPEG_INVALID_VALUE;
                reset_stream(stream);
            } else if (decoder_lock >= 0) {
                WaitSema(decoder_lock);
                for (i = 0; i < MPEG_STREAM_LIMIT; ++i)
                    if (decoder.stream == &context->streams[i])
                        close_decoder();
                SignalSema(decoder_lock);
            }
            return 0;
        }
        case 0xc132e22f:
            return 0x10000;
        case 0xd8c5f121: {
            u32 *handle = psbridge_guest_buffer(a0, sizeof(*handle));
            u32 *ring = psbridge_guest_buffer(a3, 44);
            MpegContext *context;
            if (!handle || a2 < 0x10000 || (!ring && a3) || !psbridge_guest_buffer(a1, a2))
                return MPEG_INVALID_VALUE;
            context = psbridge_guest_buffer(a1 + 0x30, sizeof(*context));
            memset(context, 0, sizeof(*context));
            memcpy(context->signature, "LIBMPEG\0" "001", sizeof(context->signature));
            context->reserved = (u32)-1;
            context->ring_address = a3;
            context->stride = a4;
            context->pixel_format = 3;
            if (ring)
                ring[10] = a0;
            *handle = a1 + 0x30;
            return 0;
        }
        case 0x606a4649: {
            MpegContext *context = mpeg_context(a0);
            if (!context)
                return MPEG_INVALID_VALUE;
            reset_streams(context);
            memset(context, 0, sizeof(*context));
            return 0;
        }
        case 0x769bebb6:
            return a0 / 2152u;
        case 0xd7a29f46:
            return a0 > 0 && a0 < 0x8000 ? a0 * 2152u : 0;
        case 0x21ff80e4:
        case 0x611e9e11: {
            const u8 *header = psbridge_guest_buffer(nid == 0x21ff80e4 ? a1 : a0, 16);
            u32 *result = psbridge_guest_buffer(nid == 0x21ff80e4 ? a2 : a1, 4);
            u32 offset;
            u32 stream_size;
            if (!header || !result || memcmp(header, "PSMF", 4))
                return (u32)-1;
            offset = big_endian_word(header + 8);
            stream_size = big_endian_word(header + 12);
            if (!offset || (offset & 2047u))
                return (u32)-1;
            if (nid == 0x21ff80e4) {
                MpegContext *context = mpeg_context(a0);
                const u8 *full_header = psbridge_guest_buffer(a1, MPEG_PACKET_BYTES);
                if (context && full_header) {
                    char key[17];
                    mpeg_key(key, full_header);
                    if (strcmp(key, context->header)) {
                        reset_streams(context);
                        memcpy(context->header, key, sizeof(key));
                        context->key[0] = 0;
                    }
                }
            }
            *result = nid == 0x21ff80e4 ? offset : stream_size;
            return 0;
        }
        case 0x42560f23: {
            MpegContext *context = mpeg_context(a0);
            if (!context)
                return MPEG_INVALID_VALUE;
            for (i = 0; i < MPEG_STREAM_LIMIT; ++i) {
                MpegStream *stream = &context->streams[i];
                if (stream->registered)
                    continue;
                *stream = (MpegStream){ .type = a1, .channel = a2, .registered = 1, .file = -1 };
                return i + 1;
            }
            return MPEG_INVALID_VALUE;
        }
        case 0x591a4aa2:
        case 0x9dcfb7ea: {
            MpegContext *context = mpeg_context(a0);
            if (!context || !a1 || a1 > MPEG_STREAM_LIMIT || !context->streams[a1 - 1].registered)
                return MPEG_INVALID_VALUE;
            if (nid == 0x591a4aa2) {
                reset_stream(&context->streams[a1 - 1]);
                context->streams[a1 - 1].registered = 0;
            } else if (a2 <= 1)
                context->streams[a1 - 1].mode = a2;
            else
                return MPEG_INVALID_VALUE;
            return 0;
        }
        case 0xa780cf7e: {
            MpegContext *context = mpeg_context(a0);
            if (!context)
                return 0;
            for (i = 0; i < 2; ++i) {
                if (context->es_buffers & (1u << i))
                    continue;
                context->es_buffers |= 1u << i;
                return i + 1;
            }
            return 0;
        }
        case 0xceb870b1: {
            MpegContext *context = mpeg_context(a0);
            if (!context || !a1 || a1 > 2)
                return MPEG_INVALID_VALUE;
            context->es_buffers &= ~(1u << (a1 - 1));
            return 0;
        }
        case 0xb5f6dc87: {
            u32 *ring = psbridge_game_pointer((void *)(uintptr_t)a0);
            if (!ring || !ring[0] || ring[0] > 8192u)
                return (u32)-1;
            return ring[0] > ring[3] ? ring[0] - ring[3] : 0;
        }
        case 0xb240a59e: {
            u32 *ring = psbridge_game_pointer((void *)(uintptr_t)a0);
            int result;
            if (!ring || !ring[0] || ring[0] > 8192u || !ring[6])
                return (u32)-1;
            u32 available = ring[0] > ring[3] ? ring[0] - ring[3] : 0;
            if (a1 > available)
                a1 = available;
            if (a1 > a2)
                a1 = a2;
            if (!a1)
                return 0;
            u32 position = ring[2] % ring[0];
            if (a1 > ring[0] - position)
                a1 = ring[0] - position;
            result = psbridge_call_callback((void *)(uintptr_t)ring[6],
                ring[5] + position * MPEG_PACKET_BYTES, a1, ring[7]);
            if (result < 0)
                return result;
            if ((u32)result > a1)
                result = (int)a1;
            MpegContext *context = mpeg_context(ring[10]);
            if (context && result > 0) {
                if (!context->packets) {
                    const u8 *first = psbridge_guest_buffer(ring[5], MPEG_PACKET_BYTES);
                    if (first)
                        find_movie(context, first);
                }
                if (context->packets <= 0xffffffffu - (u32)result)
                    context->packets += result;
                update_ring(context);
            }
            return result;
        }
        case 0xe1ce83a7:
        case 0xfe246728: {
            MpegContext *context = mpeg_context(a0);
            return get_access_unit(context, registered_stream(context, a1), a2, a3, nid == 0xe1ce83a7);
        }
        case 0x800c44df:
            return decode_audio(mpeg_context(a0), a1, a2);
        case 0x0e3c2e9d:
            return decode_video(mpeg_context(a0), a1, a2, a3, a4);
        case 0x740fccd1: {
            u32 *status = psbridge_guest_buffer(a3, 4);
            if (!mpeg_context(a0) || !status)
                return MPEG_INVALID_VALUE;
            *status = 0;
            return 0;
        }
        case 0xa11c7026: {
            MpegContext *context = mpeg_context(a0);
            const s32 *mode = psbridge_guest_buffer(a1, 8);
            if (!context || !mode || mode[1] > 3 || mode[1] < -1)
                return MPEG_INVALID_VALUE;
            if (mode[1] >= 0)
                context->pixel_format = mode[1];
            return 0;
        }
        case 0x0f6c18d7: {
            const MpegContext *context = mpeg_context(a0);
            u32 *detail = psbridge_guest_buffer(a1, 36);
            if (!context || !detail)
                return MPEG_INVALID_VALUE;
            memset(detail, 0, 36);
            detail[0] = context->result;
            detail[1] = context->frames;
            detail[2] = context->width;
            detail[3] = context->height;
            detail[8] = context->frame_ready;
            return 0;
        }
        case 0x167afd9e: {
            u32 *au = psbridge_guest_buffer(a2, 24);
            if (!mpeg_context(a0) || !au)
                return MPEG_INVALID_VALUE;
            au[0] = au[1] = au[2] = au[3] = (u32)-1;
            au[4] = a1;
            au[5] = 0;
            return 0;
        }
        case 0xf8dcb679: {
            u32 *es = psbridge_game_pointer((void *)(uintptr_t)a1);
            u32 *decoded = psbridge_game_pointer((void *)(uintptr_t)a2);
            if (es)
                *es = 2112;
            if (decoded)
                *decoded = 8192;
            return 0;
        }
        default:
            return MPEG_NO_DATA;
    }
}
