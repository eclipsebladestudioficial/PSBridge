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
#define TRANSFER_ROWS 8

static GSGLOBAL *gs;

typedef struct {
    u64 data;
    u64 reg;
} GifEntry;

static unsigned draw_buffer = 1;
static u32 transfer_pixels[TRANSFER_ROWS * PSP_BUFFER_WIDTH] __attribute__((aligned(64)));
static int graphics_ready;
static u32 frame_count;
static const void *gs_frame_source;
static int gs_frame_format;
static u32 gs_frame_hash;

int psbridge_graphics_start(void)
{
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
    gs->Width = PSP_BUFFER_WIDTH;
    gs->Height = PSP_HEIGHT;
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_32;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    gsKit_init_screen(gs);
    gs->TexturePointer = gs->CurrentPointer;

    int width = PSP_WIDTH * (gs->MagH + 1);
    int dx = gs->StartX + gs->StartXOffset + (gs->DW - width) / 2;
    int dy = gs->StartY + gs->StartYOffset;
    GS_SET_DISPLAY1(dx, dy, gs->MagH, gs->MagV, width - 1, gs->DH - 1);
    GS_SET_DISPLAY2(dx, dy, gs->MagH, gs->MagV, width - 1, gs->DH - 1);
    GS_SET_DISPFB1(gs->ScreenBuffer[0] / 8192, PSP_BUFFER_WIDTH / 64, GS_PSM_CT32, 0, 0);
    GS_SET_DISPFB2(gs->ScreenBuffer[0] / 8192, PSP_BUFFER_WIDTH / 64, GS_PSM_CT32, 0, 0);
    PSB_LOG(GE, "gs vramsinha %08x tbw %u current %08x", gs->ScreenBuffer[0], PSP_BUFFER_WIDTH / 64u, gs->CurrentPointer);
    graphics_ready = 1;
    return 0;
}

static u32 frame_hash(const u8 *source, int width, int bytes)
{
    u32 hash = 2166136261u;

    for (int y = 0; y < PSP_HEIGHT; y += 4) {
        const u8 *row = source + y * width * bytes;
        for (int x = (y >> 2) & 7; x < PSP_WIDTH; x += 8) {
            u32 pixel;
            if (bytes == 4) {
                memcpy(&pixel, row + x * sizeof(pixel), sizeof(pixel));
            } else {
                u16 pixel16;
                memcpy(&pixel16, row + x * sizeof(pixel16), sizeof(pixel16));
                pixel = pixel16;
            }
            hash = (hash ^ pixel) * 16777619u;
        }
    }

    return hash;
}

static void send_gif(const void *data, unsigned bytes)
{
    SyncDCache((void *)data, (u8 *)data + bytes);
    dmaKit_send(DMA_CHANNEL_GIF, (void *)data, bytes / 16);
    dmaKit_wait(DMA_CHANNEL_GIF, 0);
}

int psbridge_graphics_draw_target(u32 *target)
{
    if (psbridge_graphics_start() < 0)
        return -1;
    gs_frame_source = NULL;
    *target = gs->ScreenBuffer[draw_buffer];
    return 0;
}

u32 psbridge_graphics_alloc_texture(unsigned size)
{
    if (psbridge_graphics_start() < 0)
        return GSKIT_ALLOC_ERROR;
    return gsKit_vram_alloc(gs, size, GSKIT_ALLOC_USERBUFFER);
}

void psbridge_graphics_draw_invalidate(void)
{
    gs_frame_source = NULL;
}

void psbridge_graphics_draw_complete(const void *source, int format)
{
    gs_frame_hash = frame_hash(source, PSP_BUFFER_WIDTH, format == 3 ? 4 : 2);
    gs_frame_format = format;
    gs_frame_source = source;
}

static void upload_image(const u8 *pixels, unsigned qwc)
{
    GifEntry image __attribute__((aligned(64))) = { 0, 0 };

    while (qwc) {
        unsigned chunk = qwc > 0x7fff ? 0x7fff : qwc;
        image.data = GIF_TAG(chunk, 1, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        send_gif(&image, sizeof(image));
        send_gif(pixels, chunk * 16);
        pixels += chunk * 16;
        qwc -= chunk;
    }
}

static void upload_frame(const u8 *source, int width, int format, int bytes, int psm)
{
    GifEntry setup[] __attribute__((aligned(64))) = {
        { 0, GIF_AD },
        { GS_SETREG_BITBLTBUF(0, 0, 0, gs->ScreenBuffer[draw_buffer] / 256,
            PSP_BUFFER_WIDTH / 64, psm), GS_BITBLTBUF },
        { 0, GS_TRXPOS },
        { GS_SETREG_TRXREG(PSP_BUFFER_WIDTH, PSP_HEIGHT), GS_TRXREG },
        { 0, GS_TRXDIR },
    };
    setup[0].data = GIF_TAG_AD(4);
    send_gif(setup, sizeof(setup));

    if (source && (format == 1 || format == 3) && width == PSP_BUFFER_WIDTH &&
        !((uintptr_t)source & 15)) {
        upload_image(source, PSP_BUFFER_WIDTH * PSP_HEIGHT * bytes / 16);
        return;
    }

    for (int y = 0; y < PSP_HEIGHT; y += TRANSFER_ROWS) {
        for (int row = 0; row < TRANSFER_ROWS; row++) {
            u8 *destination = (u8 *)transfer_pixels + row * PSP_BUFFER_WIDTH * bytes;
            if (!source) {
                memset(destination, 0, PSP_BUFFER_WIDTH * bytes);
                continue;
            }

            const u8 *pixels = source + (y + row) * width * bytes;
            if (format == 1 || format == 3) {
                memcpy(destination, pixels, PSP_WIDTH * bytes);
            } else {
                u16 *out = (u16 *)destination;
                for (int x = 0; x < PSP_WIDTH; x++) {
                    u16 pixel;
                    memcpy(&pixel, pixels + x * sizeof(pixel), sizeof(pixel));
                    if (format == 0)
                        out[x] = (pixel & 0x1f) | ((pixel >> 1) & 0x7fe0) | 0x8000;
                    else
                        out[x] = (pixel & 15) << 1 | (pixel & 0xf0) << 2 |
                            (pixel & 0xf00) << 3 | (pixel & 0x8000) |
                            ((pixel >> 3) & 1) | ((pixel >> 2) & 0x20) |
                            ((pixel >> 1) & 0x400);
                }
            }
            memset(destination + PSP_WIDTH * bytes, 0, (PSP_BUFFER_WIDTH - PSP_WIDTH) * bytes);
        }

        upload_image((const u8 *)transfer_pixels, TRANSFER_ROWS * PSP_BUFFER_WIDTH * bytes / 16);
    }
}

void psbridge_graphics_present(void)
{
    static u32 previous_hash;
    static int previous_format = -1;
    static unsigned unchanged;
    int stride, format;

    if (psbridge_graphics_start() < 0)
        return;

    const void *source = psbridge_video_frame(&stride, &format);
    if (!source)
        format = -1;
    int bytes = format == 3 ? 4 : 2;
    u32 hash = source ? frame_hash(source, stride, bytes) : 0;

    if (format == previous_format && hash == previous_hash && frame_count &&
        (!source || ++unchanged < 8))
        return;

    unchanged = 0;
    int psm = format == 3 ? GS_PSM_CT32 : GS_PSM_CT16;
    int gs_drawn = source && source == gs_frame_source && format == gs_frame_format &&
        stride == PSP_BUFFER_WIDTH && hash == gs_frame_hash;
    gs_frame_source = NULL;

    if (!gs_drawn) {
        GifEntry finish[] __attribute__((aligned(64))) = {
            { 0, GIF_AD },
            { 0, GS_FINISH },
        };

        finish[0].data = GIF_TAG_AD(1);
        GS_SETREG_CSR_FINISH(1);
        upload_frame(source, stride, format, bytes, psm);
        send_gif(finish, sizeof(finish));
        while (!(GS_CSR_FINISH))
            __asm__ volatile("nop");
    }

    u32 base = gs->ScreenBuffer[draw_buffer] / 8192;
    GS_SET_DISPFB1(base, PSP_BUFFER_WIDTH / 64, psm, 0, 0);
    GS_SET_DISPFB2(base, PSP_BUFFER_WIDTH / 64, psm, 0, 0);
    draw_buffer ^= 1;
    previous_hash = hash;
    previous_format = format;
    psbridge_video_log_frame(++frame_count);
}
