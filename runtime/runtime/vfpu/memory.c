#include "psbridge_log.h"
#include "vfpu_internal.h"
#include <stdint.h>
#include <stdio.h>

void *vfpu_checked_pointer(u32 raw, u32 bytes)
{
    u32 a = raw & 0x3fffffff;
    void *ram;

    if ((a & 3) || a < 0x1000 || bytes > 0x10000)
        goto bad;

    if (a >= 0x04000000 && a <= 0x04180000 && bytes <= 0x04180000 - a)
        return (u8 *)psbridge_video_memory() + (a - 0x04000000);
    if ((raw & 0xe0000000u) == 0x80000000u ||
        (raw & 0xe0000000u) == 0xa0000000u) {
        if (a < 0x02000000 && bytes <= 0x02000000 - a)
            return (void *)(uintptr_t)raw;
        goto bad;
    }

    ram = psbridge_ram_pointer(raw, bytes);
    if (ram)
        return ram;
    if (a >= 0x00100000 && a < 0x02000000 && bytes <= 0x02000000 - a)
        return (void *)(uintptr_t)(a | 0x80000000u);
bad:
    if (vfpu_invalid_memory < 12)
        PSB_LOG(VFPU, "vfpu tentou gozar em ponteiro furado %08lx tamanho %lu",
               (unsigned long)raw, (unsigned long)bytes);
    vfpu_invalid_memory++;
    vfpu_last_invalid_address = raw;
    return NULL;
}
