#include <delaythread.h>
#include <kernel.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

#include "psbridge.h"

#define MUTEX_LIMIT 64
#define MUTEX_RECURSIVE 0x0200u
#define LOCK_BUSY ((int)0x800201a8u)

typedef struct {
    int lock_level;
    int lock_thread;
    int attributes;
    int waiting_threads;
    int uid;
    int padding[3];
} PspLwMutex;

typedef struct {
    int used;
    int uid;
    PspLwMutex *area;
    int semaphore;
    int guard;
    int owner;
} NativeLwMutex;

static NativeLwMutex mutexes[MUTEX_LIMIT];
static int next_mutex_uid = 0x1000;

static u64 current_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

static NativeLwMutex *find_mutex(void *workarea)
{
    PspLwMutex *area = psbridge_game_pointer(workarea);
    int index;

    if (!area)
        return NULL;

    for (index = 0; index < MUTEX_LIMIT; index++) {
        if (
            mutexes[index].used &&
            mutexes[index].area == area &&
            mutexes[index].uid == area->uid
        )
            return &mutexes[index];
    }

    return NULL;
}

int psbridge_create_lwmutex(void *workarea, u32 attributes, int initial_count)
{
    PspLwMutex *area = psbridge_game_pointer(workarea);
    ee_sema_t config;
    int semaphore;
    int guard;
    int index;

    if (!area || initial_count < 0 || (initial_count > 1 && !(attributes & MUTEX_RECURSIVE)))
        return -1;

    for (index = 0; index < MUTEX_LIMIT; index++) {
        NativeLwMutex *mutex = &mutexes[index];

        if (mutex->used)
            continue;

        memset(&config, 0, sizeof(config));
        config.init_count = initial_count ? 0 : 1;
        config.max_count = 1;
        semaphore = CreateSema(&config);

        if (semaphore < 0)
            return semaphore;

        config.init_count = 1;
        guard = CreateSema(&config);

        if (guard < 0) {
            DeleteSema(semaphore);
            return guard;
        }

        memset(mutex, 0, sizeof(*mutex));
        mutex->used = 1;
        mutex->uid = next_mutex_uid++;
        mutex->area = area;
        mutex->semaphore = semaphore;
        mutex->guard = guard;
        mutex->owner = initial_count ? GetThreadId() : -1;

        memset(area, 0, sizeof(*area));
        area->uid = mutex->uid;
        area->lock_level = initial_count;
        area->lock_thread = initial_count ? psbridge_psp_thread_uid() : 0;
        area->attributes = attributes;
        return 0;
    }

    return -1;
}

int psbridge_delete_lwmutex(void *workarea)
{
    NativeLwMutex *mutex = find_mutex(workarea);

    if (!mutex)
        return -1;

    WaitSema(mutex->guard);

    if (mutex->area->lock_level || mutex->area->waiting_threads) {
        SignalSema(mutex->guard);
        return -1;
    }

    memset(mutex->area, 0, sizeof(*mutex->area));
    mutex->used = 0;
    SignalSema(mutex->guard);
    DeleteSema(mutex->semaphore);
    DeleteSema(mutex->guard);
    return 0;
}

int psbridge_lock_lwmutex_ex(void *workarea, int count, void *timeout, int use_callbacks, int try_only)
{
    NativeLwMutex *mutex = find_mutex(workarea);
    u32 *remaining = timeout ? psbridge_game_pointer(timeout) : NULL;
    int caller = GetThreadId();
    u32 limit;
    u64 started;
    int result;

    if (!mutex || count <= 0 || (timeout && !remaining))
        return -1;

    if (count > 1 && !(mutex->area->attributes & MUTEX_RECURSIVE))
        return -1;

    limit = remaining ? *remaining : 0;
    started = current_time();
    WaitSema(mutex->guard);

    if (mutex->owner == caller && mutex->area->lock_level > 0) {
        if (
            !(mutex->area->attributes & MUTEX_RECURSIVE) ||
            mutex->area->lock_level > 0x7fffffff - count
        ) {
            SignalSema(mutex->guard);
            return -1;
        }

        mutex->area->lock_level += count;
        SignalSema(mutex->guard);
        return 0;
    }

    mutex->area->waiting_threads++;
    SignalSema(mutex->guard);

    for (;;) {
        u64 elapsed;
        u32 delay;

        result = PollSema(mutex->semaphore);

        if (result >= 0 || try_only)
            break;

        elapsed = current_time() - started;

        if (remaining && elapsed >= limit)
            break;

        if (use_callbacks)
            psbridge_check_callbacks();

        delay = remaining ? limit - (u32)elapsed : 1000;

        if (delay > 1000)
            delay = 1000;
        if (!delay)
            delay = 1;

        DelayThread(delay);
    }

    WaitSema(mutex->guard);
    mutex->area->waiting_threads--;

    if (result >= 0) {
        mutex->owner = caller;
        mutex->area->lock_thread = psbridge_psp_thread_uid();
        mutex->area->lock_level = count;
    }

    SignalSema(mutex->guard);

    if (remaining) {
        u64 elapsed = current_time() - started;
        *remaining = elapsed >= limit ? 0 : limit - (u32)elapsed;
    }

    return result >= 0 ? 0 : LOCK_BUSY;
}

int psbridge_lock_lwmutex(void *workarea, int count)
{
    return psbridge_lock_lwmutex_ex(workarea, count, NULL, 0, 0);
}

int psbridge_unlock_lwmutex(void *workarea, int count)
{
    NativeLwMutex *mutex = find_mutex(workarea);
    int release = 0;

    if (!mutex || count <= 0)
        return -1;

    WaitSema(mutex->guard);

    if (mutex->owner != GetThreadId() || mutex->area->lock_level < count) {
        SignalSema(mutex->guard);
        return -1;
    }

    mutex->area->lock_level -= count;

    if (!mutex->area->lock_level) {
        mutex->area->lock_thread = 0;
        mutex->owner = -1;
        release = 1;
    }

    if (release)
        SignalSema(mutex->semaphore);

    SignalSema(mutex->guard);
    return 0;
}


typedef struct {
    int used;
    int uid;
    int semaphore;
    int guard;
    int owner;
    int count;
    int initial_count;
    int waiters;
    char name[32];
    u32 attributes;
} NativeMutex;

#define NATIVE_MUTEX_LIMIT 32
static NativeMutex native_mutexes[NATIVE_MUTEX_LIMIT];

static NativeMutex *native_mutex_find(int uid)
{
    int index;
    for (index = 0; index < NATIVE_MUTEX_LIMIT; index++) {
        if (native_mutexes[index].used && native_mutexes[index].uid == uid)
            return &native_mutexes[index];
    }

    return NULL;
}

int psbridge_create_mutex(const char *name, u32 attributes, int initial_count, void *options)
{
    ee_sema_t config;
    NativeMutex *mutex;
    int index;
    int sema;
    int guard;
    (void)options;

    if (initial_count < 0 || (initial_count > 1 && !(attributes & MUTEX_RECURSIVE)))
        return -1;

    for (index = 0; index < NATIVE_MUTEX_LIMIT; index++) {
        if (!native_mutexes[index].used)
            break;
    }

    if (index == NATIVE_MUTEX_LIMIT)
        return -1;

    memset(&config, 0, sizeof(config));
    config.init_count = initial_count ? 0 : 1;
    config.max_count = 1;
    sema = CreateSema(&config);
    if (sema < 0)
        return sema;

    config.init_count = 1;
    guard = CreateSema(&config);
    if (guard < 0) {
        DeleteSema(sema);
        return guard;
    }

    mutex = &native_mutexes[index];
    memset(mutex, 0, sizeof(*mutex));
    mutex->used = 1;
    mutex->uid = 0x4000 + index;
    mutex->semaphore = sema;
    mutex->guard = guard;
    mutex->owner = initial_count ? GetThreadId() : -1;
    mutex->count = initial_count;
    mutex->initial_count = initial_count;
    mutex->attributes = attributes;

    if (name) {
        const char *source = psbridge_game_pointer(name);
        if (source) {
            strncpy(mutex->name, source, sizeof(mutex->name) - 1);
            mutex->name[sizeof(mutex->name) - 1] = 0;
        }
    }

    return mutex->uid;
}


typedef struct {
    u32 size;
    char name[32];
    u32 attributes;
    int initial_count;
    int current_count;
    int owner;
    int waiters;
} PspMutexStatus;

int psbridge_refer_mutex_status(int uid, void *output)
{
    NativeMutex *mutex = native_mutex_find(uid);
    PspMutexStatus *status = psbridge_game_pointer(output);
    PspMutexStatus value;
    u32 available;

    if (!mutex)
        return (int)0x800201c3u;
    if (!status)
        return (int)0x800200d3u;

    available = status->size;
    if (available < sizeof(value))
        return (int)0x800200d2u;

    WaitSema(mutex->guard);
    memset(&value, 0, sizeof(value));
    value.size = sizeof(value);
    memcpy(value.name, mutex->name, sizeof(value.name));
    value.attributes = mutex->attributes;
    value.initial_count = mutex->initial_count;
    value.current_count = mutex->count;
    value.owner = mutex->count ? mutex->owner : -1;
    value.waiters = mutex->waiters;
    SignalSema(mutex->guard);
    memcpy(status, &value, sizeof(value));
    return 0;
}

int psbridge_lock_mutex(int uid, int count, void *timeout, int try_only)
{
    NativeMutex *mutex = native_mutex_find(uid);
    u32 *remaining = timeout ? psbridge_game_pointer(timeout) : NULL;
    int caller = GetThreadId();
    int result;
    u64 started;
    u32 limit;

    if (!mutex || count < 1 || (timeout && !remaining))
        return -1;
    if (count != 1 && !(mutex->attributes & MUTEX_RECURSIVE))
        return -1;

    WaitSema(mutex->guard);
    if (mutex->owner == caller && mutex->count > 0) {
        if (!(mutex->attributes & MUTEX_RECURSIVE) || mutex->count > 0x7fffffff - count) {
            SignalSema(mutex->guard);
            return -1;
        }

        mutex->count += count;
        SignalSema(mutex->guard);
        return 0;
    }

    if (!try_only)
        mutex->waiters++;
    SignalSema(mutex->guard);

    if (try_only) {
        result = PollSema(mutex->semaphore);
    } else if (remaining) {
        limit = *remaining;
        started = current_time();
        do {
            result = PollSema(mutex->semaphore);
            if (result >= 0)
                break;
            if (current_time() - started >= limit)
                break;

            DelayThread(1000);
        } while (1);

        if (result < 0)
            *remaining = 0;
        else {
            u64 elapsed = current_time() - started;
            *remaining = elapsed < limit ? (u32)(limit - elapsed) : 0;
        }
    } else {
        result = WaitSema(mutex->semaphore);
    }

    WaitSema(mutex->guard);
    if (!try_only && mutex->waiters > 0)
        mutex->waiters--;
    if (result < 0) {
        SignalSema(mutex->guard);
        return result;
    }

    mutex->owner = caller;
    mutex->count = count;
    SignalSema(mutex->guard);
    return 0;
}

int psbridge_unlock_mutex(int uid, int count)
{
    NativeMutex *mutex = native_mutex_find(uid);
    if (!mutex || count < 1)
        return -1;

    WaitSema(mutex->guard);
    if (mutex->owner != GetThreadId() || mutex->count < count) {
        SignalSema(mutex->guard);
        return -1;
    }

    mutex->count -= count;
    if (!mutex->count) {
        mutex->owner = -1;
        SignalSema(mutex->semaphore);
    }
    SignalSema(mutex->guard);
    return 0;
}

int psbridge_delete_mutex(int uid)
{
    NativeMutex *mutex = native_mutex_find(uid);
    if (!mutex)
        return -1;

    WaitSema(mutex->guard);
    if (mutex->count) {
        SignalSema(mutex->guard);
        return -1;
    }

    mutex->used = 0;
    SignalSema(mutex->guard);
    DeleteSema(mutex->semaphore);
    DeleteSema(mutex->guard);
    return 0;
}
