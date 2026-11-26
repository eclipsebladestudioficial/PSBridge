#include "psbridge_log.h"
#include <debug.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>
#include <stdio.h>
#include <kernel.h>

#include <psbridge.h>

static u64 system_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(
        GetTimerSystemTime(),
        &seconds,
        &microseconds
    );

    return (u64)seconds * 1000000 + microseconds;
}

#define LIB_DISPLAY 0x1cebc208
#define LIB_SYSMEM 0x1902db0c
#define LIB_IO 0x67e92fd1
#define LIB_KERNEL 0xff65d9d0
#define LIB_AUDIO 0xf41677cc
#define LIB_CTRL 0x6e7a8e3b
#define LIB_UTILITY 0x241c3336
#define LIB_GE 0x412aa1a0
#define LIB_THREADMAN 0x9aacf809
#define LIB_UTILS 0xa8587892
#define LIB_UMD 0x964db0c7
#define LIB_POWER 0x892fba53
#define LIB_IMPOSE 0x8c6c6141
#define LIB_LOADEXEC 0xc469257c
#define LIB_MODULEMGR 0xe2307313
#define LIB_SUSPEND 0x1707c132
#define LIB_SAS 0x1d46fbf4
#define LIB_ATRAC 0x75263ea8
#define LIB_STDIO 0x402828c6
#define LIB_MPEG 0xe03657a3
#define LIB_DMAC 0xc9bed1c5
#define LIB_INTERRUPT 0xbdbac14f

static u64 unknown_import(u32 library, u32 nid, u32 callsite)
{
    static u32 unknown[32][2];
    static int unknown_count;
    int i;

    for (i = 0; i < unknown_count; i++) {
        if (unknown[i][0] == library && unknown[i][1] == nid)
            break;
    }

    if (i == unknown_count && unknown_count < 32) {
        unknown[unknown_count][0] = library;
        unknown[unknown_count][1] = nid;
        unknown_count++;
        PRINT_LOG("import fantasma brotou biblioteca %08lx nid %08lx chamada %08lx\n",
            (unsigned long)library, (unsigned long)nid,
            (unsigned long)callsite);
    }

    return -1;
}

static inline __attribute__((always_inline)) u64 import_service(
    u32 library, u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 callsite)
{
    if (library == LIB_INTERRUPT)
        return psbridge_subinterrupt(nid, a0, a1, a2, a3);

    if (library == LIB_SYSMEM) {
        if (nid == 0x13a5abef)
            return 0;

        if (
            nid == 0x35669d4c ||
            nid == 0xf77d77cb ||
            nid == 0x1b4217bc
        )
            return 0;

        if (nid == 0x237dbd4f) {
            return psbridge_alloc_partition(
                (int)a0,
                (const char *)a1,
                (int)a2,
                a3,
                (void *)a4
            );
        }

        if (nid == 0x9d9a5ba1)
            return (u32)(uintptr_t)psbridge_block_address((int)a0);

        if (nid == 0xb6d61d02)
            return psbridge_free_partition((int)a0);
    }

    if (library == LIB_IO) {
        if (nid == 0x55f4717d)
            return psbridge_file_chdir((const char *)(uintptr_t)a0);
        if (nid == 0xb29ddf9c)
            return psbridge_file_dopen((const char *)(uintptr_t)a0);
        if (nid == 0xe3eb004c)
            return psbridge_file_dread((int)a0, (void *)(uintptr_t)a1);
        if (nid == 0xeb092469)
            return psbridge_file_dclose((int)a0);
        if (nid == 0x109f50bc)
            return psbridge_file_open((const char *)a0, (int)a1, (int)a2);

        if (nid == 0x89aa9906)
            return psbridge_file_open_async((const char *)(uintptr_t)a0, (int)a1, (int)a2);
        if (nid == 0x35dbd746 || nid == 0xe23eec33 ||
            nid == 0x3251ea56 || nid == 0xcb05f8d6)
            return psbridge_file_async_wait((int)a0, (void *)(uintptr_t)a1,
                nid == 0x3251ea56 || nid == 0xcb05f8d6);
        if (nid == 0xa0b5a7c2)
            return psbridge_file_read_async((int)a0, (void *)(uintptr_t)a1, a2);
        if (nid == 0x0facab19)
            return psbridge_file_write_async((int)a0, (const void *)(uintptr_t)a1, a2);
        if (nid == 0xff5940b6)
            return psbridge_file_close_async((int)a0);

        if (nid == 0x810c4bc3)
            return psbridge_file_close((int)a0);

        if (nid == 0x6a638d83)
            return psbridge_file_read((int)a0, (void *)a1, a2);

        if (nid == 0x42ec03ac)
            return psbridge_file_write((int)a0, (const void *)a1, a2);

        if (nid == 0x27eb27b8)
            return psbridge_file_seek((int)a0, a2, a3, (int)a4);

        if (nid == 0x71b19e77)
            return psbridge_file_seek_async((int)a0, a2, a3, (int)a4);

        if (nid == 0x68963324)
            return (s32)psbridge_file_seek((int)a0, a1, (s32)a1 >> 31, (int)a2);

        if (nid == 0x1b385d8f)
            return psbridge_file_seek_async((int)a0, a1, (s32)a1 >> 31, (int)a2);

        if (nid == 0xace946e8)
            return psbridge_file_stat((const char *)a0, (void *)a1);

        if (nid == 0xf27a9c51)
            return psbridge_file_remove((const char *)a0);

        if (nid == 0x54f5fb11) {
            const char *device = psbridge_game_pointer((void *)a0);

            if (
                device &&
                (!strcmp(device, "umd0:") || !strcmp(device, "umd1:")) &&
                a1 == 0x01f300a7 &&
                a2 &&
                a3 >= sizeof(u32)
            ) {
                const void *cache_request = psbridge_game_pointer(
                    (void *)(uintptr_t)a2
                );

                if (cache_request)
                    return 0;
            }

            if (
                device &&
                !strcmp(device, "fatms0:") &&
                a1 == 0x2415821 &&
                a2 &&
                a3 >= 4
            )
                return 0;

            return -1;
        }

        if (nid == 0x779103a0)
            return psbridge_file_rename((const char *)a0, (const char *)a1);

        if (nid == 0xe95a012b)
            return psbridge_file_ioctl_async((int)a0, a1, (void *)(uintptr_t)a2, a3,
                (void *)(uintptr_t)a4, a5);
        if (nid == 0x63632449)
            return psbridge_file_ioctl(
                (int)a0,
                a1,
                (void *)(uintptr_t)a2,
                a3,
                (void *)(uintptr_t)a4,
                a5
            );
    }

    if (library == LIB_KERNEL) {
        if (nid == 0x293b45b8)
            return psbridge_psp_thread_uid();

        if (nid == 0x092968f4)
            return psbridge_suspend_intr();

        if (nid == 0x5f10d406 || nid == 0x3b84732d) {
            psbridge_resume_intr(a0);
            return 0;
        }

        if (nid == 0xd13bde95)
            return 0x10000;

        if (nid == 0xbea46419 || nid == 0x1fc64e09)
            return psbridge_lock_lwmutex_ex(
                (void *)a0,
                (int)a1,
                (void *)(uintptr_t)a2,
                nid == 0x1fc64e09,
                0
            );

        if (nid == 0xdc692ee3)
            return psbridge_lock_lwmutex_ex((void *)a0, (int)a1, NULL, 0, 1);

        if (nid == 0x15b6446b)
            return psbridge_unlock_lwmutex((void *)a0, (int)a1);

        if (nid == 0x1839852a) {
            void *destination = psbridge_game_pointer((void *)a0);
            const void *source = psbridge_game_pointer((void *)a1);

            if ((!destination || !source) && a2)
                return 0;

            memcpy(destination, source, a2);
            return a0;
        }

        if (nid == 0xa089eca4) {
            void *destination = psbridge_game_pointer((void *)a0);

            if (!destination && a2)
                return 0;

            memset(destination, (int)a1, a2);
            return a0;
        }
    }

    if (library == LIB_DMAC &&
        (nid == 0x617f3fe6u || nid == 0xd97f94d8u)) {
        if (!a2)
            return 0x80000104u;

        void *destination = psbridge_guest_buffer(a0, a2);
        const void *source = psbridge_guest_buffer(a1, a2);
        if (!destination || !source)
            return 0x80000103u;

        memmove(destination, source, a2);
        psbridge_graphics_draw_invalidate();
        return 0;
    }

    if (library == LIB_MPEG)
        return psbridge_video_mpeg(nid, a0, a1, a2, a3, a4, a5);

    if (library == LIB_AUDIO ||
        library == LIB_SAS ||
        library == LIB_ATRAC)
        return psbridge_audio(library, nid, a0, a1, a2, a3, a4);

    if (library == LIB_DISPLAY)
        return psbridge_display(nid, a0, a1, a2, a3);

    if (library == LIB_CTRL) {
        if (
            nid == 0x1f803938 ||
            nid == 0x3a622550 ||
            nid == 0x60b81f86 ||
            nid == 0xc152080a
        )
            return psbridge_controller_read((void *)a0, (int)a1, nid);

        if (nid == 0x1f4011e6)
            return psbridge_controller_sampling((int)a0);

        if (nid == 0xda6b76a1)
            return psbridge_controller_get_sampling((void *)(uintptr_t)a0);

        return 0;
    }

    if (library == LIB_UTILITY)
        return psbridge_utility(nid, (void *)a0);

    if (library == LIB_GE)
        return psbridge_ge(nid, a0, a1, a2, a3);

    if (library == LIB_THREADMAN) {
        if (nid == 0xd6da4ba1)
            return psbridge_create_semaphore((const char *)(uintptr_t)a0, a1, (int)a2, (int)a3);
        if (nid == 0x28b6489c)
            return psbridge_delete_semaphore((int)a0);
        if (nid == 0x3f53e640)
            return psbridge_signal_semaphore((int)a0, (int)a1);
        if (nid == 0x4e3a1105 || nid == 0x6d212bac)
            return psbridge_wait_semaphore((int)a0, (int)a1, (void *)(uintptr_t)a2,
                nid == 0x6d212bac);
        if (nid == 0xbc6febc5)
            return psbridge_refer_semaphore((int)a0, (void *)(uintptr_t)a1);
        if (nid == 0x58b1f937)
            return psbridge_poll_semaphore((int)a0, (int)a1);

        if (nid == 0x9ace131e)
            return SleepThread();
        if (nid == 0xd59ead2f)
            return psbridge_wakeup_guest_thread((int)a0);
        if (nid == 0x9fa03cd3 || nid == 0x383f7bcc)
            return psbridge_delete_guest_thread((int)a0, nid == 0x383f7bcc);
        if (nid == 0x278c0df5 || nid == 0x840e8133)
            return psbridge_wait_guest_thread_end((int)a0,
                (void *)(uintptr_t)a1, nid == 0x840e8133);

        if (nid == 0x56c039b5)
            return psbridge_create_vpl((const char *)(uintptr_t)a0, (int)a1, a2, a3, (void *)(uintptr_t)a4);

        if (nid == 0xbed27435 || nid == 0xaf36d708)
            return psbridge_allocate_vpl((int)a0, a1, (void *)(uintptr_t)a2, nid == 0xaf36d708);

        if (nid == 0xb736e9ff)
            return psbridge_free_vpl((int)a0, (void *)(uintptr_t)a1);

        if (nid == 0x89b3d48c)
            return psbridge_delete_vpl((int)a0);

        if (nid == 0x39810265)
            return psbridge_refer_vpl_status((int)a0, (void *)(uintptr_t)a1);

        if (nid == 0xa9c2cb9a)
            return psbridge_refer_mutex_status((int)a0, (void *)(uintptr_t)a1);

        if (nid == 0xb7d098c6)
            return psbridge_create_mutex(
                (const char *)(uintptr_t)a0, a1, (int)a2, (void *)(uintptr_t)a3
            );

        if (nid == 0xb011b11f || nid == 0x5bf4dd27 || nid == 0x0ddcd2c9)
            return psbridge_lock_mutex(
                (int)a0, (int)a1, (void *)(uintptr_t)a2, nid == 0x0ddcd2c9
            );

        if (nid == 0x6b30100f)
            return psbridge_unlock_mutex((int)a0, (int)a1);

        if (nid == 0xf8170fbe)
            return psbridge_delete_mutex((int)a0);

        if (nid == 0x71bc9871)
            return psbridge_change_thread_priority((int)a0, (int)a1);

        if (nid == 0x94aa61ee)
            return psbridge_current_thread_priority();

        if (nid == 0x17c1684e)
            return psbridge_refer_thread_status((int)a0, (void *)(uintptr_t)a1);

        if (nid == 0xaa73c935 || nid == 0x809ce29b) {
            psbridge_exit_guest_thread((int)a0);
            return 0;
        }


        if (nid == 0xea748e31) {
            return 0;
        }

        if (nid == 0x8125221d)
            return psbridge_create_mailbox(
                (const char *)a0,
                a1,
                (void *)a2
            );

        if (nid == 0x86255ada)
            return psbridge_delete_mailbox((int)a0);

        if (nid == 0xe9b3061e)
            return psbridge_send_mailbox(
                (int)a0,
                (void *)a1
            );

        if (nid == 0xf3986382 ||
            nid == 0x18260574
        )
            return psbridge_receive_mailbox(
                (int)a0,
                (void *)a1,
                (void *)a2
            );

        if (nid == 0xbc80ec7c)
            return psbridge_call_guest((void *)a1, (void *)a2);

        if (nid == 0xe81caf8f)
            return psbridge_create_callback(a1, a2);

        if (nid == 0xedba5844)
            return psbridge_delete_callback((int)a0);

        if (nid == 0xc11ba8c4)
            return psbridge_notify_callback((int)a0, a1);

        if (nid == 0xba4051d6)
            return psbridge_cancel_callback((int)a0);

        if (nid == 0x2a3d44ff)
            return psbridge_callback_count((int)a0);

        if (nid == 0x349d6d6c)
            return psbridge_check_callbacks();

        if (nid == 0x55c20a00)
            return psbridge_create_event((const char *)(uintptr_t)a0, a1, a2);

        if (nid == 0xa66b0120)
            return psbridge_refer_event_status((int)a0, (void *)(uintptr_t)a1);

        if (nid == 0xef9e4c70)
            return psbridge_delete_event((int)a0);

        if (nid == 0x1fb15a32)
            return psbridge_set_event((int)a0, a1);

        if (nid == 0x812346e4)
            return psbridge_clear_event((int)a0, a1);

        if (nid == 0x402fcf22 || nid == 0x328c546a)
            return psbridge_wait_event(
                (int)a0,
                a1,
                a2,
                (void *)(uintptr_t)a3,
                (void *)(uintptr_t)a4,
                nid == 0x328c546a,
                0
            );

        if (nid == 0x30fd48f0)
            return psbridge_wait_event(
                (int)a0,
                a1,
                a2,
                (void *)(uintptr_t)a3,
                NULL,
                0,
                1
            );

        if (
            nid == 0xceadeb47 ||
            nid == 0x68da9e36
        )
            return psbridge_delay_thread(a0, nid == 0x68da9e36);

        if (nid == 0x369ed59d)
            return (u32)system_time();

        if (nid == 0x19cff145)
            return psbridge_create_lwmutex((void *)a0, a2, (int)a3);

        if (nid == 0x60107536)
            return psbridge_delete_lwmutex((void *)a0);

        if (nid == 0xc07bb470)
            return psbridge_create_fpl(
                (const char *)a0,
                (int)a1,
                a2,
                a3,
                a4,
                (void *)a5
            );

        if (nid == 0x623ae665)
            return psbridge_allocate_fpl((int)a0, (void **)a1);

        if (nid == 0xed1410e0)
            return psbridge_delete_fpl((int)a0);

        if (nid == 0x446d8de6) {
            return psbridge_create_thread(
                (const char *)a0,
                (void *)a1,
                (int)a2,
                (int)a3,
                a4,
                (void *)a5
            );
        }

        if (nid == 0xf475845d)
            return psbridge_start_thread((int)a0, a1, (void *)a2);

        if (nid == 0x82bc5777)
            return system_time();
    }

    if (library == LIB_LOADEXEC && nid == 0x4ac57943)
        return 0;

    if (library == LIB_MODULEMGR) {
        if (nid == 0xb7f46618)
            return psbridge_load_firmware_module((int)a0);

        if (nid == 0x977de386) {
            int descriptor = psbridge_file_open((const char *)a0, 1, 0);
            int module;

            if (descriptor < 0)
                return descriptor;

            module = psbridge_load_firmware_module(descriptor);
            psbridge_file_close(descriptor);
            return module;
        }

        if (nid == 0x50f0c1ec || nid == 0xd1ff982a ||
            nid == 0x2e0911aa)
            return psbridge_change_firmware_module((int)a0, nid, a3);

        if (nid == 0xf0a26395)
            return 1;

        if (nid == 0xd8b73127) {
            u32 address = a0 & 0x3fffffff;
            if (address >= guest_info.base &&
                address - guest_info.base < guest_info.memory_size)
                return 1;

            return -1;
        }

        if (nid == 0x8f2df740)
            return 0;
    }

    if (library == LIB_POWER) {
        if (
            nid == 0x04b7766e ||
            nid == 0x469989ad
        )
            return 0;

        if (nid == 0xa85880d0)
            return 1;
    }

    if (library == LIB_IMPOSE)
        return 0;

    if (library == LIB_UTILS) {
        if (nid == 0x71ec4271) {
            u32 *timeval = a0 ? psbridge_game_pointer((void *)(uintptr_t)a0) : NULL;
            u32 *timezone = a1 ? psbridge_game_pointer((void *)(uintptr_t)a1) : NULL;
            u64 clock = system_time();
            if (a0 && !timeval)
                return -1;

            if (timeval) {
                timeval[0] = (u32)(clock / 1000000) + 946684800u;
                timeval[1] = (u32)(clock % 1000000);
            }
            if (timezone) {
                timezone[0] = 0;
                timezone[1] = 0;
            }
            return 0;
        }

        if (nid == 0x91e4f6a7)
            return (u32)system_time();
        if (nid == 0x27cc57f0) {
            u32 now = (u32)(system_time() / 1000000) + 946684800u;
            if (a0) {
                u32 *output = psbridge_game_pointer((void *)(uintptr_t)a0);
                if (!output)
                    return -1;

                *output = now;
            }

            return now;
        }

        if (nid == 0xb435dec5) {
            FlushCache(0);
            return 0;
        }

        if (nid == 0x3ee30821)
            return 0;

        if (nid == 0x79d1c3fa)
            return 0;

        if (
            nid == 0xc2df770e ||
            nid == 0x920f104a
        )
            return 0;
    }

    if (library == LIB_STDIO) {
        if (nid == 0x172d316e)
            return 0;
        if (nid == 0xa6bab2e9)
            return 1;
        if (nid == 0xf78ba90a)
            return 2;
    }

    if (library == LIB_UMD) {
        if (nid == 0x46ebb729)
            return psbridge_umd_present();

        if (nid == 0x6b4a146c)
            return psbridge_umd_state();

        if (nid == 0xc6183d47)
            return psbridge_umd_activate((int)a0, (const char *)a1);

        if (nid == 0xe83742ba)
            return psbridge_umd_deactivate((int)a0, (const char *)a1);

        if (nid == 0x8ef08fce)
            return psbridge_umd_wait(a0);

        if (nid == 0x4a9e5e29)
            return psbridge_umd_wait_cb(a0, a1);

        if (nid == 0xaee7404d)
            return psbridge_umd_register_callback((int)a0);

        if (nid == 0xbd2bde07)
            return psbridge_umd_unregister_callback((int)a0);
    }

    if (library == LIB_SUSPEND) {
        if (nid == 0x090ccb3f)
            return 0;

        if (nid == 0x3e0271d3)
            return psbridge_volatile_lock((void *)a1, (void *)a2);

        if (nid == 0xa569e425)
            return psbridge_volatile_unlock();
    }

    return unknown_import(library, nid, callsite);
}

#define PSBRIDGE_IMPORT(symbol, library, nid) \
    u64 symbol(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 callsite) \
    { \
        psbridge_vblank_interrupts(); \
        return import_service(library, nid, a0, a1, a2, a3, a4, a5, callsite); \
    }

#include "game.imports.inc"

#undef PSBRIDGE_IMPORT
