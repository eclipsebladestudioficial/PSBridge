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
    init_scr();

    PRINT_LOG("PSBridge levantou a frica\n");
    PRINT_LOG("memoria do jogo %u bytes\n", guest_info.memory_size);
    PRINT_LOG("heap do jogo gozando %u bytes\n", (u32)((uintptr_t)__psbridge_heap_end - (uintptr_t)__psbridge_heap_start));
    PRINT_LOG("imports na fila do sus %u\n", psp_import_count);

    memset(
        (u8 *)guest_info.memory + guest_info.file_size,
        0,
        guest_info.memory_size - guest_info.file_size
    );

    FlushCache(0);
    FlushCache(2);

    if (psbridge_map_game_memory() < 0)
        return -1;

    if (psbridge_sync_init() < 0 || psbridge_memory_init() < 0) {
        PRINT_LOG("memoria nao conseguiu abrir a tranca da putaria\n");
        return -1;
    }

    PRINT_LOG("pulando pro jogo\n");
    int result = psbridge_start();
    PRINT_LOG("o jogo voltou com %d\n", result);

    SleepThread();
    return result;
}
