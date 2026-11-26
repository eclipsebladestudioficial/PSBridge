#include "psbridge_log.h"
#include <debug.h>
#include <kernel.h>
#include <delaythread.h>
#include <tamtypes.h>
#include <string.h>
#include <sifrpc.h>
#include <psbridge.h>
#include <stdio.h>
#include <stdint.h>

extern void *_gp;
extern unsigned char __psbridge_heap_start[];
extern unsigned char __psbridge_heap_end[];

u32 main_kernel_work[0x400] __attribute__((aligned(16)));

extern int psbridge_memory_init(void);

int main(void) 
{
    sceSifInitRpc(0);

    PSB_LOG(BOOT, "PSBridge levantou a frica");
    PSB_LOG(BOOT, "memoria do jogo %u bytes", guest_info.memory_size);
    PSB_LOG(BOOT, "heap do jogo gozando %u bytes", (u32)((uintptr_t)__psbridge_heap_end - (uintptr_t)__psbridge_heap_start));
    PSB_LOG(BOOT, "imports na fila do sus %u", psp_import_count);

    memset(
        (u8 *)guest_info.memory + guest_info.file_size,
        0,
        guest_info.memory_size - guest_info.file_size
    );

    FlushCache(0);
    FlushCache(2);

    if (psbridge_map_game_memory() < 0)
        return -1;

    if (psbridge_sync_init() < 0 || psbridge_memory_init() < 0 || psbridge_graphics_init() < 0) {
        PSB_LOG(BOOT, "memoria nao conseguiu abrir a tranca da putaria");
        return -1;
    }

    PSB_LOG(BOOT, "pulando pro jogo");

    int result = psbridge_start();
    PSB_LOG(BOOT, "o jogo voltou com %d", result);

    SleepThread();
    return result;
}
