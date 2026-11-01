#include <string.h>

#include "ge/ge_internal.h"

GeList ge_lists[GE_LIST_LIMIT];
GeState ge_state;
int ge_blend_mode_seen;
volatile u32 ge_ready_buffer;
volatile u32 ge_ready_sequence;

u32 psbridge_ge_ready_buffer(void)
{
    return ge_ready_buffer;
}

u32 psbridge_ge_ready_sequence(void)
{
    return ge_ready_sequence;
}

u64 psbridge_ge(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3)
{
    GeList *list;
    int index;

    if (nid == 0xe47e40e4)
        return (u32)(uintptr_t)psbridge_video_memory();

    if (nid == 0x1f6752ad) {
        static unsigned vramsinha;
        if (vramsinha++ < 4)
            PRINT_LOG("ge contou a vramsinha %u bytes\n", 0x200000u);
        return 0x200000u;
    }

    if (nid == 0xa4fc06a4)
        return (u32)register_ge_callback(a0);

    if (nid == 0x05db22ce) {
        GeCallback *cb = find_ge_callback((int)a0);
        if (!cb)
            return (u32)-1;
        memset(cb, 0, sizeof(*cb));
        return 0;
    }

    if (nid == 0xab49e76a || nid == 0x1c0d95a6)
        return enqueue_list(a0, a1, (int)a2);

    if (nid == 0xe0d68148) {
        list = find_list((int)a0);

        if (!list)
            return -1;

        list->stall = a1 & 0x3fffffff;
        execute_list(list);
        return 0;
    }

    if (nid == 0x03444eb4) {
        list = find_list((int)a0);

        if (!list)
            return -1;

        execute_list(list);
        return list->done ? 0 : 3;
    }

    if (nid == 0xb287bd61) {
        for (index = 0; index < GE_LIST_LIMIT; index++) {
            if (ge_lists[index].used)
                execute_list(&ge_lists[index]);
        }

        return 0;
    }

    (void)a2;
    (void)a3;
    return 0;
}
