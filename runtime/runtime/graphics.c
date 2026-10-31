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
static u16 display_pixels[GS_OUTPUT_WIDTH * GS_OUTPUT_HEIGHT]
    __attribute__((aligned(128)));
#define frame_pixels display_pixels
static int graphics_ready;
static u32 frame_count;

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
    frame_texture.Filter = GS_FILTER_LINEAR;
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
    PSB_LOG(GE, "gs vramsinha %08x tbw %u current %08x", frame_texture.Vram, frame_texture.TBW, gs->CurrentPointer);
    graphics_ready = 1;
    return 0;
}

void psbridge_graphics_present(void)
{
    int x;
    int y;

    if (psbridge_graphics_start() < 0)
        return;

    psbridge_video_render_frame(frame_pixels);
    psbridge_video_log_frame(++frame_count);
    FlushCache(0);

    for (y = GS_OUTPUT_HEIGHT - 1; y >= 0; y--) {
        const u16 *src = frame_pixels + (y * PSP_HEIGHT / GS_OUTPUT_HEIGHT) * PSP_BUFFER_WIDTH;
        u16 *dst = display_pixels + y * GS_OUTPUT_WIDTH;

        for (x = GS_OUTPUT_WIDTH - 1; x >= 0; x--)
            dst[x] = src[x * PSP_WIDTH / GS_OUTPUT_WIDTH];
    }

    FlushCache(0);

    gsKit_texture_send(
        (u32 *)((uintptr_t)display_pixels & 0x1fffffff),
        GS_OUTPUT_WIDTH,
        GS_OUTPUT_HEIGHT,
        gs->ScreenBuffer[0],
        GS_PSM_CT16,
        GS_OUTPUT_WIDTH / 64,
        0
    );
}
