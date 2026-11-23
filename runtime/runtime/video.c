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

extern inline __attribute__((always_inline)) u32 psbridge_subinterrupt(u32 nid, u32 interrupt, u32 slot, u32 entry, u32 argument)
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

const void *psbridge_video_frame(int *stride, int *format)
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

    *format = framebuffer_format;
    *stride = 0;
    if (!source || (unsigned)framebuffer_format > 3)
        return NULL;

    if (width < PSP_WIDTH || width > 2048)
        width = PSP_BUFFER_WIDTH;

    unsigned bytes = framebuffer_format == 3 ? 4 : 2;
    if (visible_offset < VIDEO_MEMORY_SIZE &&
        ((PSP_HEIGHT - 1) * width + (width == PSP_BUFFER_WIDTH ? width : PSP_WIDTH)) * bytes > VIDEO_MEMORY_SIZE - visible_offset)
        return NULL;

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

    *stride = width;
    return source;
}

void psbridge_video_log_frame(u32 frame_count)
{
    if (frame_count <= 4 || frame_count % 120 == 0) {
        const u32 *pixels = (const u32 *)framebuffer_pointer(framebuffer);
        PSB_LOG(VIDEO, "frame gozando numero %u endereco %08x largura %d formato %d pixel %08x", frame_count, (u32)(uintptr_t)framebuffer, framebuffer_width, framebuffer_format, pixels ? pixels[0] : 0);
    }
}

extern inline __attribute__((always_inline)) u64 psbridge_display(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3)
{
    if (nid == 0xdba6c4c4) {
        float rate = 59.9400599f;
        u32 bits;
        memcpy(&bits, &rate, sizeof(bits));
        return bits;
    }

    if (nid == 0x0e20f177) {
        psbridge_graphics_start();
        return 0;
    }

    if (nid == 0x289d82fe) {
        display_set_count++;
        if (display_set_count <= 20 || display_set_count % 100 == 0) PSB_LOG(VIDEO, "display trocou a frica %u endereco %08x stride %u formato %u", display_set_count, a0, a1, a2);
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

    if (nid == 0x210eab3a)
        return (display_elapsed(current_time()) * DISPLAY_LINES_PER_FRAME /
                VBLANK_DURATION_US) & 0x7fffffffu;

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
