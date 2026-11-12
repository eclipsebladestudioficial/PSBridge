#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>
#include <stdint.h>
#include <string.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272
#define PSP_BUFFER_WIDTH 512
#define GS_OUTPUT_WIDTH 640
#define GS_OUTPUT_HEIGHT 448

static GSGLOBAL *gs;
static GSTEXTURE frame_texture;
static GSTEXTURE frame_texture32;
static int texture32_ready;
static u16 frame_pixels[PSP_BUFFER_WIDTH * PSP_HEIGHT]
    __attribute__((aligned(128)));
static int graphics_ready;
static u32 frame_count;
extern int psbridge_video_frame_modified(void);
extern const u32 *psbridge_video_direct_frame32(void);

int psbridge_graphics_start(void)
{
    u32 texture_size;

    if (graphics_ready)
        return 0;

    gs = gsKit_init_global_custom(128 * 1024, 32 * 1024);

    if (!gs)
        return -1;

    dmaKit_init(
        D_CTRL_RELE_OFF,
        D_CTRL_MFD_OFF,
        D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF,
        D_CTRL_RCYC_8,
        1 << DMA_CHANNEL_GIF
    );
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gs->Mode = GS_MODE_NTSC;
    gs->Interlace = GS_INTERLACED;
    gs->Field = GS_FIELD;
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_OFF;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ATEST_OFF);
    gs->TexturePointer = gs->CurrentPointer;

    memset(&frame_texture, 0, sizeof(frame_texture));
    frame_texture.Width = PSP_BUFFER_WIDTH;
    frame_texture.Height = PSP_HEIGHT;
    frame_texture.PSM = GS_PSM_CT16;
    frame_texture.Filter = GS_FILTER_NEAREST;
    frame_texture.Mem = (u32 *)frame_pixels;

    texture_size = gsKit_texture_size(
        frame_texture.Width,
        frame_texture.Height,
        frame_texture.PSM
    );
    frame_texture.Vram = gsKit_vram_alloc(
        gs,
        texture_size,
        GSKIT_ALLOC_USERBUFFER
    );

    if (frame_texture.Vram == GSKIT_ALLOC_ERROR)
        return -1;

    gsKit_setup_tbw(&frame_texture);
    memset(&frame_texture32, 0, sizeof(frame_texture32));
    frame_texture32.Width = PSP_BUFFER_WIDTH;
    frame_texture32.Height = PSP_HEIGHT;
    frame_texture32.PSM = GS_PSM_CT32;
    frame_texture32.Filter = GS_FILTER_NEAREST;
    frame_texture32.Vram = gsKit_vram_alloc(
        gs,
        gsKit_texture_size(PSP_BUFFER_WIDTH, PSP_HEIGHT, GS_PSM_CT32),
        GSKIT_ALLOC_USERBUFFER
    );
    if (frame_texture32.Vram != GSKIT_ALLOC_ERROR) {
        gsKit_setup_tbw(&frame_texture32);
        texture32_ready = 1;
    }
    PSB_LOG(GE, "gs vramsinha %08x tbw %u current %08x", frame_texture.Vram, frame_texture.TBW, gs->CurrentPointer);
    graphics_ready = 1;
    return 0;
}

void psbridge_graphics_present(int submitted)
{
    if (psbridge_graphics_start() < 0)
        return;

    if (!submitted) {
        static u32 idle;
        if (!psbridge_video_frame_modified() && ++idle < 24)
            return;
        idle = 0;
    } else {
        psbridge_video_frame_modified();
    }

    const u32 *direct = texture32_ready ? psbridge_video_direct_frame32() : NULL;
    const GSTEXTURE *texture = &frame_texture;
    if (direct) {
        FlushCache(0);
        gsKit_texture_send_inline(
            gs,
            (u32 *)((uintptr_t)direct & 0x1fffffff),
            PSP_BUFFER_WIDTH, PSP_HEIGHT, frame_texture32.Vram,
            GS_PSM_CT32, frame_texture32.TBW, 0
        );
        texture = &frame_texture32;
    } else {
        psbridge_video_render_frame(frame_pixels);
        FlushCache(0);
        gsKit_texture_send_inline(
            gs,
            (u32 *)((uintptr_t)frame_pixels & 0x1fffffff),
            PSP_BUFFER_WIDTH, PSP_HEIGHT, frame_texture.Vram,
            GS_PSM_CT16, frame_texture.TBW, 0
        );
    }
    psbridge_video_log_frame(++frame_count);
    gsKit_prim_sprite_texture_3d(
        gs,
        texture,
        0.0f, 0.0f, 1, 0.0f, 0.0f,
        (float)GS_OUTPUT_WIDTH, (float)GS_OUTPUT_HEIGHT, 1,
        (float)PSP_WIDTH, (float)PSP_HEIGHT,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0)
    );
    gsKit_queue_exec(gs);
    gsKit_queue_reset(gs->Os_Queue);
    if (submitted)
        gsKit_display_buffer(gs);
}
