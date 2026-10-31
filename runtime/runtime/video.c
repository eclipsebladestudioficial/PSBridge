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

static u64 current_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

static u32 current_vcount(void)
{
    u64 now = current_time();

    if (!vblank_ready) {
        vblank_epoch = now;
        vblank_ready = 1;
    }

    return (u32)((now - vblank_epoch) / VBLANK_DURATION_US);
}

static void wait_vblank(u32 cycles, int use_callbacks)
{
    u64 now = current_time();
    u64 current;
    u64 target;

    if (!vblank_ready) {
        vblank_epoch = now;
        vblank_ready = 1;
    }

    if (!cycles)
        cycles = 1;
    if (cycles > 1000)
        cycles = 1000;

    current = (now - vblank_epoch) / VBLANK_DURATION_US;
    target = vblank_epoch + (current + cycles) * VBLANK_DURATION_US;

    while ((now = current_time()) < target) {
        u64 remaining = target - now;

        if (remaining > 3000)
            remaining = 3000;

        if (use_callbacks)
            psbridge_delay_thread((u32)remaining, 1);
        else
            DelayThread((int)remaining);
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
        psbridge_graphics_present();
        return 0;
    }

    if (nid == 0x9c6eaad7)
        return current_vcount();

    if (
        nid == 0x984c27e7 ||
        nid == 0x46f186c3 ||
        nid == 0x77ed8b3a
    ) {
        u32 cycles = nid == 0x77ed8b3a ? a0 : 1;

        psbridge_graphics_present();
        wait_vblank(cycles, nid != 0x984c27e7);
        return 0;
    }

    return 0;
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
        case 0x606a4649:
        case 0x13407f13:
        case 0x591a4aa2:
        case 0x707b7629:
        case 0xceb870b1:
        case 0x4571cc64:
        case 0x740fccd1:
        case 0x0e3c2e9d:
            return 0;
        case 0xc132e22f:
            return 0x10000;
        case 0xd8c5f121:
            return 0;
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
        case 0x42560f23:
            return (a1 & 0xffu) + 1;
        case 0xa780cf7e:
            return 1;
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
        case 0x167afd9e: {
            u32 *au = psbridge_game_pointer((void *)(uintptr_t)a2);
            if (au)
                memset(au, 0, 24);
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

