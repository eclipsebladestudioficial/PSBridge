#include <delaythread.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272
#define PSP_BUFFER_WIDTH 512
#define VIDEO_MEMORY_SIZE 0x200000

static unsigned char video_memory[VIDEO_MEMORY_SIZE] __attribute__((aligned(64)));

void *psbridge_video_memory(void)
{
    return video_memory;
}

static const void *framebuffer;
static int framebuffer_width = PSP_BUFFER_WIDTH;
static int framebuffer_format = 3;
static u64 vblank_epoch;
static int vblank_ready;
static u32 display_set_count;
static u32 ge_frame_seq_seen;
static u32 last_display_vblank;
static u32 frame_saved_hash;
static u32 auto_source_offset;
static int auto_source_active;
static u32 known_video_buffers[2];
static int known_video_count;

extern u32 psbridge_ge_ready_buffer(void);
extern u32 psbridge_ge_ready_sequence(void);

#define VBLANK_DURATION_US 16683u
#define DISPLAY_LINES_PER_FRAME 286u
#define VBLANK_INTERRUPT 30u
#define VBLANK_USER_SLOTS 16u

typedef struct {
    u32 entry;
    u32 argument;
} VblankHandler;

static VblankHandler vblank_handlers[VBLANK_USER_SLOTS];
static u32 vblank_enabled;
static u32 vblank_next;

static u64 current_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

static u64 display_elapsed(u64 now)
{
    if (!vblank_ready) {
        vblank_epoch = now;
        vblank_ready = 1;
    }

    return now - vblank_epoch;
}

static u32 current_vcount(void)
{
    return (u32)(display_elapsed(current_time()) / VBLANK_DURATION_US);
}

void psbridge_vblank_interrupts(void)
{
    if (!vblank_enabled || (s32)(current_vcount() - vblank_next) < 0 ||
        !psbridge_try_enter_intr())
        return;

    u32 count = current_vcount();
    if ((s32)(count - vblank_next) >= 0) {
        vblank_next = count + 1;
        for (u32 slot = 0; slot < VBLANK_USER_SLOTS; slot++) {
            VblankHandler handler = vblank_handlers[slot];
            if ((vblank_enabled & (1u << slot)) && handler.entry)
                psbridge_call_callback((void *)(uintptr_t)handler.entry, slot, handler.argument, 0);
        }
    }

    psbridge_resume_intr(1);
}

u32 psbridge_subinterrupt(u32 nid, u32 interrupt, u32 slot, u32 entry, u32 argument)
{
    if (interrupt != VBLANK_INTERRUPT)
        return 0x80020068u;
    if (slot >= VBLANK_USER_SLOTS)
        return 0x80020065u;

    u32 flags = psbridge_suspend_intr();
    u32 bit = 1u << slot;
    u32 result = 0;
    VblankHandler *handler = &vblank_handlers[slot];

    switch (nid) {
        case 0xca04a2b9:
            if (handler->entry) {
                result = 0x80020067u;
            } else {
                handler->entry = entry;
                handler->argument = argument;
            }
            break;
        case 0xd61e6961:
            if (!handler->entry) {
                result = 0x80020068u;
            } else {
                handler->entry = 0;
                vblank_enabled &= ~bit;
            }
            break;
        case 0xfb8e22ec:
            if (!vblank_enabled)
                vblank_next = current_vcount() + 1;
            vblank_enabled |= bit;
            break;
        case 0x8a389411:
            vblank_enabled &= ~bit;
            break;
        default:
            result = 0x80020068u;
            break;
    }

    psbridge_resume_intr(flags);
    return result;
}

static void wait_vblank(u32 cycles, int use_callbacks)
{
    u64 now = current_time();
    u64 current;
    u64 target;

    if (!cycles)
        cycles = 1;
    if (cycles > 1000)
        cycles = 1000;

    current = display_elapsed(now) / VBLANK_DURATION_US;
    target = vblank_epoch + (current + cycles) * VBLANK_DURATION_US;

    while ((now = current_time()) < target) {
        u64 remaining = target - now;

        if (remaining > 3000)
            remaining = 3000;

        if (use_callbacks)
            psbridge_delay_thread((u32)remaining, 1);
        else
            DelayThread((int)remaining);

        psbridge_vblank_interrupts();
    }
}

static u16 gs_color(u8 red, u8 green, u8 blue, u8 alpha)
{
    return
        (red >> 3) |
        ((green >> 3) << 5) |
        ((blue >> 3) << 10) |
        ((alpha >= 0x80) << 15);
}

static const void *framebuffer_pointer(const void *address)
{
    u32 value = (u32)(uintptr_t)address;
    u32 video = (u32)(uintptr_t)psbridge_video_memory();

    if (value & 0x40000000)
        value &= ~0x40000000;

    if (value >= 0x04000000 && value < 0x04200000)
        return (u8 *)psbridge_video_memory() + value - 0x04000000;

    if (value < 0x00200000)
        return (u8 *)psbridge_video_memory() + value;

    if (value >= video && value < video + 0x00200000)
        return (const void *)(uintptr_t)value;

    return psbridge_game_pointer((const void *)(uintptr_t)value);
}

static u16 convert_pixel(const void *source, u32 index, int format)
{
    u16 pixel16;
    u32 pixel32;
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;

    if (format == 3) {
        pixel32 = ((const u32 *)source)[index];
        return gs_color(
            pixel32,
            pixel32 >> 8,
            pixel32 >> 16,
            pixel32 >> 24
        );
    }

    pixel16 = ((const u16 *)source)[index];

    if (format == 0) {
        red = (pixel16 & 0x1f) << 3;
        green = ((pixel16 >> 5) & 0x3f) << 2;
        blue = ((pixel16 >> 11) & 0x1f) << 3;
        return gs_color(red, green, blue, 0xff);
    }

    if (format == 1)
        return pixel16;

    red = (pixel16 & 0xf) * 17;
    green = ((pixel16 >> 4) & 0xf) * 17;
    blue = ((pixel16 >> 8) & 0xf) * 17;
    alpha = ((pixel16 >> 12) & 0xf) * 17;
    return gs_color(red, green, blue, alpha);
}

static u32 framebuffer_hash(const void *ptr, int width)
{
    const u32 *source = (const u32 *)ptr;
    u32 hash = 2166136261u;
    int x;
    int y;

    if (!source || width < PSP_WIDTH || width > 2048)
        return 0;
    for (y = 8; y < PSP_HEIGHT; y += 16) {
        for (x = 8; x < PSP_WIDTH; x += 16) {
            hash ^= source[y * width + x];
            hash *= 16777619u;
        }
    }
    return hash;
}

int psbridge_video_frame_modified(void)
{
    static const void *last_pointer;
    static u32 last_hash;
    static u32 last_sequence;
    static int last_width;
    static int ready;
    const void *source = framebuffer_pointer(framebuffer);
    const u8 *vram = (const u8 *)psbridge_video_memory();
    uintptr_t offset;
    u32 hash;
    u32 sequence;
    int changed;

    if (!source || framebuffer_format != 3 ||
        framebuffer_width < PSP_WIDTH || framebuffer_width > 2048) {
        ready = 0;
        return 1;
    }
    if ((uintptr_t)source < (uintptr_t)vram) {
        ready = 0;
        return 1;
    }
    offset = (uintptr_t)source - (uintptr_t)vram;
    if (offset > VIDEO_MEMORY_SIZE ||
        (u32)framebuffer_width * PSP_HEIGHT * 4u > VIDEO_MEMORY_SIZE - offset) {
        ready = 0;
        return 1;
    }

    hash = framebuffer_hash(source, framebuffer_width);
    sequence = psbridge_ge_ready_sequence();
    changed = !ready || last_pointer != source ||
        last_hash != hash || last_sequence != sequence ||
        last_width != framebuffer_width;
    ready = 1;
    last_pointer = source;
    last_hash = hash;
    last_sequence = sequence;
    last_width = framebuffer_width;
    return changed;
}

const u32 *psbridge_video_direct_frame32(void)
{
    const void *source = framebuffer_pointer(framebuffer);
    int width = framebuffer_width;
    u32 ge_sequence = psbridge_ge_ready_sequence();
    u32 ge_offset = psbridge_ge_ready_buffer();
    u32 display_vblank = current_vcount();
    u8 *vram = (u8 *)psbridge_video_memory();
    u32 visible_offset = (u32)((uintptr_t)source - (uintptr_t)vram);
    const void *candidate = NULL;
    u32 candidate_hash = 0;
    u32 current_hash = 0;

    if (!source)
        return NULL;

    if (width < PSP_WIDTH || width > 2048)
        width = PSP_BUFFER_WIDTH;

    if (framebuffer_format == 3 && visible_offset <= 0x200000u - 0x88000u) {
        current_hash = framebuffer_hash(source, width);
        if (ge_offset <= 0x200000u - 0x88000u && ge_offset != visible_offset && ge_sequence) {
            int known = 0;
            int i;
            for (i = 0; i < known_video_count; i++) {
                if (known_video_buffers[i] == ge_offset) {
                    known = 1;
                    break;
                }
            }
            if (known) {
                candidate = vram + ge_offset;
                candidate_hash = framebuffer_hash(candidate, PSP_BUFFER_WIDTH);
            }
        }
        if (candidate && !auto_source_active && ge_sequence != ge_frame_seq_seen &&
            current_hash == frame_saved_hash && candidate_hash != current_hash &&
            display_vblank - last_display_vblank >= 12u) {
            auto_source_active = 1;
            auto_source_offset = ge_offset;
                }
        if (auto_source_active && auto_source_offset == ge_offset &&
            display_vblank - last_display_vblank >= 12u) {
            source = vram + auto_source_offset;
            width = PSP_BUFFER_WIDTH;
        }
        frame_saved_hash = current_hash;
        ge_frame_seq_seen = ge_sequence;
    }

    if (framebuffer_format != 3 || width != PSP_BUFFER_WIDTH ||
        ((uintptr_t)source & 15u))
        return NULL;
    return (const u32 *)source;
}

void psbridge_video_render_frame(u16 *frame_pixels)
{
    const void *source = framebuffer_pointer(framebuffer);
    int width = framebuffer_width;
    u32 ge_sequence = psbridge_ge_ready_sequence();
    u32 ge_offset = psbridge_ge_ready_buffer();
    u32 display_vblank = current_vcount();
    u8 *vram = (u8 *)psbridge_video_memory();
    u32 visible_offset = (u32)((uintptr_t)source - (uintptr_t)vram);
    const void *candidate = NULL;
    u32 candidate_hash = 0;
    u32 current_hash = 0;
    int x;
    int y;

    memset(frame_pixels, 0, PSP_BUFFER_WIDTH * PSP_HEIGHT * sizeof(u16));

    if (!source)
        return;

    if (width < PSP_WIDTH || width > 2048)
        width = PSP_BUFFER_WIDTH;

    if (framebuffer_format == 3 && visible_offset <= 0x200000u - 0x88000u) {
        current_hash = framebuffer_hash(source, width);
        if (ge_offset <= 0x200000u - 0x88000u && ge_offset != visible_offset && ge_sequence) {
            int known = 0;
            int i;
            for (i = 0; i < known_video_count; i++) {
                if (known_video_buffers[i] == ge_offset) {
                    known = 1;
                    break;
                }
            }
            if (known) {
                candidate = vram + ge_offset;
                candidate_hash = framebuffer_hash(candidate, PSP_BUFFER_WIDTH);
            }
        }
        if (candidate && !auto_source_active && ge_sequence != ge_frame_seq_seen &&
            current_hash == frame_saved_hash && candidate_hash != current_hash &&
            display_vblank - last_display_vblank >= 12u) {
            auto_source_active = 1;
            auto_source_offset = ge_offset;
                }
        if (auto_source_active && auto_source_offset == ge_offset &&
            display_vblank - last_display_vblank >= 12u) {
            source = vram + auto_source_offset;
            width = PSP_BUFFER_WIDTH;
        }
        frame_saved_hash = current_hash;
        ge_frame_seq_seen = ge_sequence;
    }

    if (framebuffer_format == 1) {
        for (y = 0; y < PSP_HEIGHT; y++) {
            memcpy(
                frame_pixels + y * PSP_BUFFER_WIDTH,
                (const u16 *)source + y * width,
                PSP_WIDTH * sizeof(u16)
            );
        }

        return;
    }

    for (y = 0; y < PSP_HEIGHT; y++) {
        for (x = 0; x < PSP_WIDTH; x++) {
            u16 pixel = convert_pixel(
                source,
                y * width + x,
                framebuffer_format
            );

            frame_pixels[y * PSP_BUFFER_WIDTH + x] = pixel;
        }
    }
}

void psbridge_video_log_frame(u32 frame_count)
{
    if (frame_count <= 4 || frame_count % 120 == 0) {
        const u32 *pixels = (const u32 *)framebuffer_pointer(framebuffer);
        PSB_LOG(GE, "frame gozando numero %u endereco %08x largura %d formato %d pixel %08x", frame_count, (u32)(uintptr_t)framebuffer, framebuffer_width, framebuffer_format, pixels ? pixels[0] : 0);
    }
}

u64 psbridge_display(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3)
{
    if (nid == 0x0e20f177) {
        psbridge_graphics_start();
        return 0;
    }

    if (nid == 0x289d82fe) {
        display_set_count++;
        if (display_set_count <= 20 || display_set_count % 100 == 0) PSB_LOG(GE, "display trocou a frica %u endereco %08x stride %u formato %u", display_set_count, a0, a1, a2);
        framebuffer = (const void *)(uintptr_t)a0;
        last_display_vblank = current_vcount();
        auto_source_active = 0;
        {
            const void *resolved = framebuffer_pointer(framebuffer);
            u32 base = (u32)(uintptr_t)psbridge_video_memory();
            u32 address = (u32)(uintptr_t)resolved;
            if (resolved && address >= base && address - base <= 0x200000u - 0x88000u) {
                u32 offset = address - base;
                int i;
                int known = 0;
                for (i = 0; i < known_video_count; i++) {
                    if (known_video_buffers[i] == offset)
                        known = 1;
                }
                if (!known && known_video_count < 2)
                    known_video_buffers[known_video_count++] = offset;
            }
        }
        framebuffer_width = a1;
        framebuffer_format = a2;
        (void)a3;
        psbridge_graphics_present(1);
        return 0;
    }

    if (nid == 0x9c6eaad7)
        return current_vcount();

    if (nid == 0x210eab3a)
        return (display_elapsed(current_time()) * DISPLAY_LINES_PER_FRAME /
                VBLANK_DURATION_US) & 0x7fffffffu;

    if (
        nid == 0x984c27e7 ||
        nid == 0x46f186c3 ||
        nid == 0x77ed8b3a
    ) {
        u32 cycles = nid == 0x77ed8b3a ? a0 : 1;

        psbridge_graphics_present(0);
        wait_vblank(cycles, nid != 0x984c27e7);
        return 0;
    }

    return 0;
}

#define MPEG_STREAM_LIMIT 32
#define MPEG_INVALID_VALUE 0x806101feu

typedef struct {
    u32 type;
    u32 channel;
    u32 mode;
    int registered;
} MpegStream;

typedef struct {
    char signature[12];
    u32 reserved;
    u32 ring_address;
    u32 ring_data_end;
    u32 es_buffers;
    MpegStream streams[MPEG_STREAM_LIMIT];
} MpegContext;

static void *mpeg_pointer(u32 address, u32 size)
{
    u32 physical = address & 0x1fffffffu;
    if ((address >= 0x80000000u && address < 0xc0000000u) &&
        physical >= 0x100000u && physical < 0x2000000u && size <= 0x2000000u - physical)
        return (void *)(uintptr_t)address;
    return psbridge_ram_pointer(address, size);
}

static MpegContext *mpeg_context(u32 address)
{
    const u32 *handle = mpeg_pointer(address, sizeof(*handle));
    MpegContext *context = handle ? mpeg_pointer(*handle, sizeof(*context)) : NULL;
    return context && !memcmp(context->signature, "LIBMPEG", 8) ? context : NULL;
}

u64 psbridge_video_mpeg(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    static u32 seen[40];
    static int count;
    int i;
    for (i = 0; i < count; i++) {
        if (seen[i] == nid)
            break;
    }
    if (i == count && count < 40) {
        seen[count++] = nid;
        PRINT_LOG("video foi de arrasta nid %08lx\n", (unsigned long)nid);
    }

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
        case 0x682a619b:
        case 0x874624d6:
        case 0x13407f13:
        case 0x707b7629:
        case 0x4571cc64:
            return 0;
        case 0xc132e22f:
            return 0x10000;
        case 0xd8c5f121: {
            u32 *handle = mpeg_pointer(a0, sizeof(*handle));
            u32 *ring = mpeg_pointer(a3, 44);
            MpegContext *context;
            if (!handle || a2 < 0x10000 || (!ring && a3) || !mpeg_pointer(a1, a2))
                return MPEG_INVALID_VALUE;
            context = mpeg_pointer(a1 + 0x30, sizeof(*context));
            memset(context, 0, sizeof(*context));
            memcpy(context->signature, "LIBMPEG\0" "001", sizeof(context->signature));
            context->reserved = (u32)-1;
            context->ring_address = a3;
            if (ring) {
                context->ring_data_end = ring[8];
                ring[10] = a0;
            }
            *handle = a1 + 0x30;
            return 0;
        }
        case 0x606a4649: {
            MpegContext *context = mpeg_context(a0);
            if (!context)
                return MPEG_INVALID_VALUE;
            memset(context, 0, sizeof(*context));
            return 0;
        }
        case 0xd7a29f46:
            return a0 > 0 && a0 < 0x8000 ? a0 * 2152u : 0;
        case 0x21ff80e4:
        case 0x611e9e11: {
            const u8 *header = psbridge_game_pointer((void *)(uintptr_t)(nid == 0x21ff80e4 ? a1 : a0));
            u32 *result = psbridge_game_pointer((void *)(uintptr_t)(nid == 0x21ff80e4 ? a2 : a1));
            u32 offset;
            u32 stream_size;
            if (!header || !result || memcmp(header, "PSMF", 4))
                return (u32)-1;
            offset = ((u32)header[8] << 24) | ((u32)header[9] << 16) |
                     ((u32)header[10] << 8) | header[11];
            stream_size = ((u32)header[12] << 24) | ((u32)header[13] << 16) |
                          ((u32)header[14] << 8) | header[15];
            if (!offset || (offset & 2047u))
                return (u32)-1;
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
                *stream = (MpegStream){ .type = a1, .channel = a2, .registered = 1 };
                return i + 1;
            }
            return MPEG_INVALID_VALUE;
        }
        case 0x591a4aa2:
        case 0x9dcfb7ea: {
            MpegContext *context = mpeg_context(a0);
            if (!context || !a1 || a1 > MPEG_STREAM_LIMIT || !context->streams[a1 - 1].registered)
                return MPEG_INVALID_VALUE;
            if (nid == 0x591a4aa2)
                context->streams[a1 - 1] = (MpegStream){0};
            else if (a2 <= 1)
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
            return ring[0];
        }
        case 0xb240a59e: {
            u32 *ring = psbridge_game_pointer((void *)(uintptr_t)a0);
            int result;
            if (!ring || !ring[0] || ring[0] > 8192u || !ring[6])
                return (u32)-1;
            if (a1 > ring[0])
                a1 = ring[0];
            if (a1 > a2)
                a1 = a2;
            if (!a1)
                return 0;
            result = psbridge_call_callback((void *)(uintptr_t)ring[6], ring[5], a1, ring[7]);
            if (result < 0)
                return result;
            if ((u32)result > a1)
                result = (int)a1;
            return result;
        }
        case 0xe1ce83a7:
        case 0xfe246728:
            return 0x80618001u;
        case 0x800c44df:
            return 0x80618001u;
        case 0x0e3c2e9d:
        case 0x740fccd1: {
            u32 *status = mpeg_pointer(nid == 0x0e3c2e9d ? a4 : a3, sizeof(*status));
            if (!mpeg_context(a0) || !status)
                return MPEG_INVALID_VALUE;
            *status = 0;
            return nid == 0x740fccd1 ? 0 : 0x80618001u;
        }
        case 0x167afd9e: {
            u32 *au = mpeg_pointer(a2, 24);
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
            return 0x80618001u;
    }
}
