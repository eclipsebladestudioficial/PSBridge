#include <delaythread.h>
#include <kernel.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define EVENT_LIMIT 32
#define CALLBACK_LIMIT 32
#define WAIT_TIMEOUT ((int)0x800201a8u)
#define BAD_ID ((int)0x8002019au)

typedef struct {
    int used;
    int uid;
    u32 attributes;
    u32 pattern;
} EventFlag;

typedef struct {
    int used;
    int uid;
    int thread;
    u32 entry;
    u32 common;
    u32 pending;
    u32 argument;
} Callback;

static EventFlag event_flags[EVENT_LIMIT];
static Callback callbacks[CALLBACK_LIMIT];
static int next_event_uid = 0x2000;
static int next_callback_uid = 0x1800;
static int sync_lock = -1;
static int intr_gate = -1;
static volatile int intr_owner = -1;
static volatile u32 intr_depth;

u32 psbridge_suspend_intr(void)
{
    int current = GetThreadId();

    if (intr_owner == current) {
        intr_depth++;
        return 0;
    }

    if (intr_gate < 0 || WaitSema(intr_gate) < 0)
        return 1;

    intr_owner = current;
    intr_depth = 1;
    return 1;
}

void psbridge_resume_intr(u32 flags)
{
    int current = GetThreadId();

    if (intr_owner != current || !intr_depth)
        return;

    intr_depth--;
    if (intr_depth || !flags)
        return;

    intr_owner = -1;
    SignalSema(intr_gate);
}

static u64 current_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

int psbridge_sync_init(void)
{
    ee_sema_t config;

    if (sync_lock >= 0)
        return 0;

    memset(&config, 0, sizeof(config));
    config.init_count = 1;
    config.max_count = 1;
    sync_lock = CreateSema(&config);
    if (sync_lock < 0)
        return sync_lock;

    intr_gate = CreateSema(&config);
    if (intr_gate < 0) {
        int result = intr_gate;
        DeleteSema(sync_lock);
        sync_lock = -1;
        return result;
    }
    return 0;
}

static EventFlag *find_event(int uid)
{
    int index;

    for (index = 0; index < EVENT_LIMIT; index++) {
        if (event_flags[index].used && event_flags[index].uid == uid)
            return &event_flags[index];
    }

    return NULL;
}

static Callback *find_callback(int uid)
{
    int index;

    for (index = 0; index < CALLBACK_LIMIT; index++) {
        if (callbacks[index].used && callbacks[index].uid == uid)
            return &callbacks[index];
    }

    return NULL;
}

int psbridge_create_event(u32 attributes, u32 initial)
{
    int index;
    int uid;

    if (psbridge_sync_init() < 0)
        return -1;

    WaitSema(sync_lock);

    for (index = 0; index < EVENT_LIMIT; index++) {
        if (event_flags[index].used)
            continue;

        uid = next_event_uid++;
        event_flags[index].used = 1;
        event_flags[index].uid = uid;
        event_flags[index].attributes = attributes;
        event_flags[index].pattern = initial;
        SignalSema(sync_lock);
        return uid;
    }

    SignalSema(sync_lock);
    return -1;
}

int psbridge_delete_event(int uid)
{
    EventFlag *event;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    event = find_event(uid);

    if (!event) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    memset(event, 0, sizeof(*event));
    SignalSema(sync_lock);
    return 0;
}

int psbridge_set_event(int uid, u32 bits)
{
    EventFlag *event;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    event = find_event(uid);

    if (!event) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    event->pattern |= bits;
    SignalSema(sync_lock);
    return 0;
}

int psbridge_clear_event(int uid, u32 mask)
{
    EventFlag *event;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    event = find_event(uid);

    if (!event) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    event->pattern &= mask;
    SignalSema(sync_lock);
    return 0;
}

int psbridge_wait_event(int uid, u32 bits, u32 mode, void *output, void *timeout, int use_callbacks, int polling)
{
    u32 *result = output ? psbridge_game_pointer(output) : NULL;
    u32 *remaining = timeout ? psbridge_game_pointer(timeout) : NULL;
    u32 limit = remaining ? *remaining : 0;
    u64 started = current_time();

    if ((!result && output) || (!remaining && timeout) || !bits || (mode & ~0x31u))
        return -1;

    if (sync_lock < 0)
        return BAD_ID;

    for (;;) {
        EventFlag *event;
        u32 pattern;
        u64 elapsed;
        int ready;

        WaitSema(sync_lock);
        event = find_event(uid);

        if (!event) {
            SignalSema(sync_lock);
            return BAD_ID;
        }

        pattern = event->pattern;
        ready = (mode & 1) ? (pattern & bits) != 0 : (pattern & bits) == bits;

        if (ready) {
            if (mode & 0x10)
                event->pattern = 0;
            else if (mode & 0x20)
                event->pattern &= ~bits;
        }

        SignalSema(sync_lock);

        if (ready) {
            if (result)
                *result = pattern;

            if (remaining) {
                elapsed = current_time() - started;
                *remaining = elapsed >= limit ? 0 : limit - (u32)elapsed;
            }

            return 0;
        }

        if (polling)
            return (int)0x800201afu;

        elapsed = current_time() - started;

        if (remaining && elapsed >= limit) {
            *remaining = 0;
            return WAIT_TIMEOUT;
        }

        if (use_callbacks)
            psbridge_check_callbacks();

        {
            u32 delay = remaining ? limit - (u32)elapsed : 1000;

            if (delay > 1000)
                delay = 1000;
            if (!delay)
                delay = 1;

            DelayThread(delay);
        }
    }
}

int psbridge_create_callback(u32 function, u32 common)
{
    int index;

    if (!function || psbridge_sync_init() < 0)
        return -1;

    WaitSema(sync_lock);

    for (index = 0; index < CALLBACK_LIMIT; index++) {
        Callback *callback = &callbacks[index];

        if (callback->used)
            continue;

        callback->used = 1;
        callback->uid = next_callback_uid++;
        callback->thread = GetThreadId();
        callback->entry = function;
        callback->common = common;
        callback->pending = 0;
        callback->argument = 0;
        SignalSema(sync_lock);
        return callback->uid;
    }

    SignalSema(sync_lock);
    return -1;
}

int psbridge_delete_callback(int uid)
{
    Callback *callback;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    callback = find_callback(uid);

    if (!callback) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    memset(callback, 0, sizeof(*callback));
    SignalSema(sync_lock);
    return 0;
}

int psbridge_notify_callback(int uid, u32 argument)
{
    Callback *callback;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    callback = find_callback(uid);

    if (!callback) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    if (callback->pending < 0x7fffffff)
        callback->pending++;

    callback->argument = argument;
    SignalSema(sync_lock);

    return 0;
}

int psbridge_cancel_callback(int uid)
{
    Callback *callback;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    callback = find_callback(uid);

    if (!callback) {
        SignalSema(sync_lock);
        return BAD_ID;
    }

    callback->pending = 0;
    SignalSema(sync_lock);
    return 0;
}

int psbridge_callback_count(int uid)
{
    Callback *callback;
    int count;

    if (sync_lock < 0)
        return BAD_ID;

    WaitSema(sync_lock);
    callback = find_callback(uid);
    count = callback ? (int)callback->pending : BAD_ID;
    SignalSema(sync_lock);
    return count;
}

int psbridge_check_callbacks(void)
{
    int current = GetThreadId();
    int fired = 0;
    int index;

    if (sync_lock < 0)
        return 0;

    for (index = 0; index < CALLBACK_LIMIT; index++) {
        Callback *callback = &callbacks[index];
        u32 function;
        u32 count;
        u32 argument;
        u32 common;
        int uid;
        int result;

        WaitSema(sync_lock);

        if (!callback->used || callback->thread != current || !callback->pending) {
            SignalSema(sync_lock);
            continue;
        }

        uid = callback->uid;
        function = callback->entry;
        count = callback->pending;
        argument = callback->argument;
        common = callback->common;
        callback->pending = 0;
        SignalSema(sync_lock);

        result = psbridge_call_callback(
            (void *)(uintptr_t)function,
            count,
            argument,
            common
        );

        fired++;

        if (result)
            psbridge_delete_callback(uid);
    }

    return fired;
}

int psbridge_delay_thread(u32 delay, int use_callbacks)
{
    u64 started;

    if (!use_callbacks)
        return DelayThread((int)delay);

    started = current_time();
    psbridge_check_callbacks();

    for (;;) {
        u64 elapsed = current_time() - started;
        u32 remaining;

        if (elapsed >= delay)
            return 0;

        remaining = delay - (u32)elapsed;

        if (remaining > 2000)
            remaining = 2000;

        DelayThread(remaining);
        psbridge_check_callbacks();
    }
}


#define PSP_SEMA_LIMIT 32

typedef struct {
    int used;
    int uid;
    int semaphore;
    int guard;
    int count;
    int maximum;
} NativePspSema;

static NativePspSema psp_semas[PSP_SEMA_LIMIT];



static NativePspSema *find_psp_sema(int uid)
{
    int i;
    for (i = 0; i < PSP_SEMA_LIMIT; i++) {
        if (psp_semas[i].used && psp_semas[i].uid == uid)
            return &psp_semas[i];
    }
    return NULL;
}

int psbridge_create_semaphore(const char *name, u32 attributes, int initial, int maximum)
{
    ee_sema_t cfg;
    NativePspSema *sema;
    int idx;
    int native;
    int guard;
    (void)name;
    (void)attributes;

    if (maximum < 1 || initial < 0 || initial > maximum)
        return -1;
    for (idx = 0; idx < PSP_SEMA_LIMIT; idx++) {
        if (!psp_semas[idx].used)
            break;
    }
    if (idx == PSP_SEMA_LIMIT)
        return -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.init_count = 0;
    cfg.max_count = 1;
    native = CreateSema(&cfg);
    if (native < 0)
        return native;
    cfg.init_count = 1;
    guard = CreateSema(&cfg);
    if (guard < 0) {
        DeleteSema(native);
        return guard;
    }
    sema = &psp_semas[idx];
    memset(sema, 0, sizeof(*sema));
    sema->used = 1;
    sema->uid = 0x6000 + idx;
    sema->semaphore = native;
    sema->guard = guard;
    sema->count = initial;
    sema->maximum = maximum;
    return sema->uid;
}

int psbridge_delete_semaphore(int uid)
{
    NativePspSema *sema = find_psp_sema(uid);
    if (!sema)
        return -1;
    sema->used = 0;
    DeleteSema(sema->semaphore);
    DeleteSema(sema->guard);
    return 0;
}

int psbridge_signal_semaphore(int uid, int count)
{
    NativePspSema *sema = find_psp_sema(uid);
    int result;

    if (!sema || count <= 0)
        return -1;

    result = WaitSema(sema->guard);
    if (result < 0)
        return result;

    if (count > sema->maximum - sema->count) {
        SignalSema(sema->guard);
        return -1;
    }

    sema->count += count;
    SignalSema(sema->guard);
    return 0;
}

int psbridge_wait_semaphore(int uid, int count, void *timeout)
{
    NativePspSema *sema = find_psp_sema(uid);
    u32 *remaining = timeout ? psbridge_game_pointer(timeout) : NULL;
    u32 limit = remaining ? *remaining : 0;
    u64 started = current_time();
    int warned = 0;

    if (!sema || count <= 0 || count > sema->maximum || (timeout && !remaining))
        return -1;

    for (;;) {
        u64 elapsed;
        u32 delay;
        int result;
        int available;

        result = WaitSema(sema->guard);
        if (result < 0)
            return result;
        if (!sema->used || sema->uid != uid) {
            SignalSema(sema->guard);
            return -1;
        }
        available = sema->count >= count;
        if (available)
            sema->count -= count;
        SignalSema(sema->guard);

        elapsed = current_time() - started;
        if (available) {
            if (remaining)
                *remaining = elapsed >= limit ? 0 : limit - (u32)elapsed;
            return 0;
        }
        if (remaining && elapsed >= limit) {
            *remaining = 0;
            return WAIT_TIMEOUT;
        }
        if (!warned && elapsed >= 500000) {
            warned = 1;
            PRINT_LOG("sema esperando a putaria uid %d pediu %d tem %d tempo %u\n",
                uid, count, sema->count, (u32)elapsed);
        }
        delay = remaining ? limit - (u32)elapsed : 1000;
        if (delay > 1000)
            delay = 1000;
        DelayThread(delay ? delay : 1);
    }
}
