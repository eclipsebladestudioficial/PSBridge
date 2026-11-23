#include <delaythread.h>
#include <kernel.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define THREAD_ARG_SIZE 256
#define THREAD_LIMIT 64
#define THREAD_STACK_MIN 0x4000
#define THREAD_STACK_OVERHEAD 0x2000
#define THREAD_STACK_MAX 0x100000
#define KERNEL_STACK_SIZE 0x1000
#define MAILBOX_LIMIT 8
#define MAILBOX_MESSAGES 32

typedef struct {
    int used;
    int uid;
    int thread_id;
    char name[32];
    u32 attributes;
    void *entry;
    PspThreadWait wait;
    u32 arg_size;
    void *arg;
    unsigned char arg_copy[THREAD_ARG_SIZE];
    void *kernel_work;
    void *stack_buffer;
    u32 stack_capacity;
    u32 stack_size;
} PspThread;

typedef struct {
    u32 size;
    char name[32];
    u32 attributes;
    u32 status;
    u32 entry;
    u32 stack;
    u32 stack_size;
    u32 gp;
    int initial_priority;
    int current_priority;
    u32 wait_type;
    int wait_uid;
    u32 wakeup_count;
    int exit_status;
    u32 run_clocks[2];
    u32 interrupt_preempts;
    u32 thread_preempts;
    u32 releases;
    u32 reserved;
} PspThreadStatus;

typedef struct {
    int used;
    int uid;
    int semaphore;
    int lock;
    u32 messages[MAILBOX_MESSAGES];
    int read_index;
    int write_index;
    int count;
} PspMailbox;

extern void *_gp;

extern int psbridge_call_thread(
    void *entry,
    u32 arg_size,
    void *arg,
    void *kernel_work
);

typedef struct {
    int uid;
    int status;
} FinishedPspThread;

#define FINISHED_THREAD_LIMIT 128

static PspThread threads[THREAD_LIMIT];
static PspThreadWait main_thread_wait;
static FinishedPspThread finished_threads[FINISHED_THREAD_LIMIT];
static u32 finished_thread_count;
static int next_thread_uid = 0x100;
static PspMailbox mailboxes[MAILBOX_LIMIT];
static int next_mailbox_uid = 0x3000;

static PspThread *find_thread(int uid);


static void finish_guest_thread(int uid, int status)
{
    FinishedPspThread *finished = &finished_threads[
        finished_thread_count % FINISHED_THREAD_LIMIT
    ];

    finished->uid = uid;
    finished->status = status;
    finished_thread_count++;
}

static int finished_guest_thread(int uid, int *status)
{
    u32 index;

    for (index = 0; index < FINISHED_THREAD_LIMIT; index++) {
        if (finished_threads[index].uid == uid) {
            *status = finished_threads[index].status;
            return 1;
        }
    }

    return 0;
}

static u64 guest_thread_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

int psbridge_wait_guest_thread_end(int uid, void *timeout, int use_callbacks)
{
    u32 *remaining = timeout ? psbridge_game_pointer(timeout) : NULL;
    u32 limit = remaining ? *remaining : 0;
    u64 started = guest_thread_time();
    int warned = 0;
    int status;

    if ((timeout && !remaining) || uid == psbridge_psp_thread_uid())
        return -1;

    for (;;) {
        PspThread *thread = find_thread(uid);
        u64 elapsed;

        if (!thread) {
            if (!finished_guest_thread(uid, &status))
                return -1;

            elapsed = guest_thread_time() - started;
            if (remaining)
                *remaining = elapsed >= limit ? 0 : limit - (u32)elapsed;
            {
                static u32 logs;
                if (logs++ < 32)
                    PSB_LOG(THREAD, "thread esperou a putaria uid %d retorno %d tempo %u",
                        uid, status, (u32)elapsed);
            }
            return status;
        }

        elapsed = guest_thread_time() - started;
        if (remaining && elapsed >= limit) {
            *remaining = 0;
            return (int)0x800201a8u;
        }

        if (!warned && elapsed >= 500000) {
            warned = 1;
            PSB_LOG(THREAD, "thread ainda ta esperando a putaria uid %d ee %d tempo %u",
                uid, thread->thread_id, (u32)elapsed);
        }

        if (use_callbacks)
            psbridge_check_callbacks();

        if (remaining && limit - (u32)elapsed < 1000)
            DelayThread(limit - (u32)elapsed ? limit - (u32)elapsed : 1);
        else
            DelayThread(1000);
    }
}

static PspThread *find_thread(int uid)
{
    int index;

    for (index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used > 0 && threads[index].uid == uid)
            return &threads[index];
    }
    return NULL;
}

static PspThread *find_thread_by_host_id(int id)
{
    for (int index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used > 0 && threads[index].thread_id == id)
            return &threads[index];
    }
    return NULL;
}

PspThreadWait psbridge_set_thread_wait(PspThreadWait wait)
{
    PspThread *thread = find_thread_by_host_id(GetThreadId());
    PspThreadWait *current = thread ? &thread->wait : &main_thread_wait;
    int interrupt_state = DIntr();
    PspThreadWait previous = *current;

    *current = wait;
    if (interrupt_state)
        EIntr();
    return previous;
}

int psbridge_count_thread_waits(u32 type, int uid)
{
    int count = main_thread_wait.type == type && main_thread_wait.uid == uid;

    for (int index = 0; index < THREAD_LIMIT; index++) {
        PspThread *thread = &threads[index];
        if (thread->used > 0 && thread->wait.type == type && thread->wait.uid == uid)
            count++;
    }
    return count;
}

static PspMailbox *find_mailbox(int uid)
{
    int index;

    for (index = 0; index < MAILBOX_LIMIT; index++) {
        if (mailboxes[index].used && mailboxes[index].uid == uid)
            return &mailboxes[index];
    }

    return NULL;
}

static void run_thread(void *arg)
{
    PspThread *thread = arg;
    u32 *kernel_work = thread->kernel_work;
    u32 *initial_stack = (u32 *)thread->stack_buffer;

    memset(kernel_work, 0, KERNEL_STACK_SIZE);

    kernel_work[0xc0 / 4] = thread->uid;
    kernel_work[0xc8 / 4] = (u32) (uintptr_t) initial_stack;
    kernel_work[0xf8 / 4] = 0xffffffff;
    kernel_work[0xfc / 4] = 0xffffffff;
    initial_stack[0] = thread->uid;

    {
        int result = psbridge_call_thread(
            thread->entry,
            thread->arg_size,
            thread->arg,
            thread->kernel_work
        );
        static unsigned logs;
        if (logs++ < 32)
            PSB_LOG(THREAD, "thread %d terminou a putaria retorno %d", thread->uid, result);
        finish_guest_thread(thread->uid, result);
    }

    thread->used = 0;
    ExitDeleteThread();

}

int psbridge_create_thread(const char *name, void *entry, int priority, int stack_size, u32 attributes, void *options)
{
    int interrupt_state;
    u32 needed;
    u32 capacity;
    PspThread *thread = NULL;
    ee_thread_t config;
    int thread_id;
    (void)options;

    if (stack_size < 0 || stack_size > THREAD_STACK_MAX)
        return -1;
    needed = ((u32)stack_size + THREAD_STACK_OVERHEAD + 15) & ~15u;
    if (needed < THREAD_STACK_MIN)
        needed = THREAD_STACK_MIN;
    capacity = needed + KERNEL_STACK_SIZE;

    interrupt_state = DIntr();
    for (int index = 0; index < THREAD_LIMIT; index++) {
        PspThread *candidate = &threads[index];

        if (candidate->used)
            continue;
        if (!thread ||
            (candidate->stack_capacity >= capacity &&
             (thread->stack_capacity < capacity || candidate->stack_capacity < thread->stack_capacity)) ||
            (candidate->stack_capacity < capacity && candidate->stack_capacity > thread->stack_capacity))
            thread = candidate;
    }
    if (thread)
        thread->used = -1;
    if (interrupt_state)
        EIntr();
    if (!thread)
        return -1;

    if (thread->stack_capacity < capacity) {
        void *stack = psbridge_resize_thread_stack(thread->stack_buffer, capacity);

        if (!stack) {
            thread->used = 0;
            return -1;
        }
        thread->stack_buffer = stack;
        thread->stack_capacity = capacity;
    }

    if (priority < 2)
        priority = 2;
    if (priority > 126)
        priority = 126;

    memset(&config, 0, sizeof(config));
    config.func = run_thread;
    config.stack = thread->stack_buffer;
    config.stack_size = needed;
    config.gp_reg = &_gp;
    config.initial_priority = priority;

    thread_id = CreateThread(&config);
    if (thread_id < 0) {
        thread->used = 0;
        return thread_id;
    }

    thread->thread_id = thread_id;
    name = name ? psbridge_game_pointer(name) : NULL;
    memset(thread->name, 0, sizeof(thread->name));
    if (name)
        strncpy(thread->name, name, sizeof(thread->name) - 1);
    thread->attributes = attributes;
    thread->entry = entry;
    thread->wait = (PspThreadWait){0};
    thread->stack_size = needed;
    thread->kernel_work = (u8 *)thread->stack_buffer + needed;
    interrupt_state = DIntr();
    thread->uid = next_thread_uid++;
    thread->used = 1;
    if (interrupt_state)
        EIntr();

    return thread->uid;
}

int psbridge_start_thread(int uid, u32 arg_size, void *arg)
{
    PspThread *thread = find_thread(uid);
    u32 *words;
    int result;

    if (!thread)
        return -1;

    thread->arg_size = arg_size;
    thread->arg = NULL;

    if (arg_size && arg) {
        u32 copy_size = arg_size;

        if (copy_size > THREAD_ARG_SIZE)
        copy_size = THREAD_ARG_SIZE;

        memcpy(
            thread->arg_copy,
            psbridge_game_pointer(arg),
            copy_size
        );

        thread->arg_size = copy_size;
        thread->arg = thread->arg_copy;
    }

    words = thread->arg;

    PSB_LOG(
        THREAD,
        "thread %d abriu o puteiro argumento %08x tamanho %u bagulho %08x %08x %08x %08x",
        uid,
        (u32)(uintptr_t)arg,
        thread->arg_size,
        words && thread->arg_size >= 4 ? words[0] : 0,
        words && thread->arg_size >= 8 ? words[1] : 0,
        words && thread->arg_size >= 12 ? words[2] : 0,
        words && thread->arg_size >= 16 ? words[3] : 0
    );

    result = StartThread(thread->thread_id, thread);

    if (result < 0)
        return result;

    return 0;
}

int psbridge_create_mailbox(const char *name, u32 attributes, void *options)
{
    int index;
    (void)name;
    (void)attributes;
    (void)options;

    for (index = 0; index < MAILBOX_LIMIT; index++) {
        PspMailbox *mailbox;
        ee_sema_t config;
        int semaphore;
        int lock;

        if (mailboxes[index].used)
            continue;

        memset(&config, 0, sizeof(config));

        config.init_count = 0;
        config.max_count = MAILBOX_MESSAGES;

        semaphore = CreateSema(&config);

        if (semaphore < 0)
             return semaphore;
           

        memset(&config, 0, sizeof(config));
        config.init_count = 1;
        config.max_count = 1;

        lock = CreateSema(&config);

        if (lock < 0) {
            DeleteSema(semaphore);
            return lock;
        }

        mailbox = &mailboxes[index];
        memset(mailbox, 0, sizeof(*mailbox));

        mailbox->used = 1;
        mailbox->uid = next_mailbox_uid++;
        mailbox->semaphore = semaphore;
        mailbox->lock = lock;

        return mailbox->uid;


    }

    return -1;
}

int psbridge_send_mailbox(int uid, void *message)
{
    PspMailbox *mailbox = find_mailbox(uid);
    static u32 log_count;
    int result;

    if (!mailbox || !message)
        return -1;

    if (!psbridge_game_pointer(message))
        return -1;

    result = WaitSema(mailbox->lock);

    if (result < 0)
        return result;

    if (mailbox->count >= MAILBOX_MESSAGES) {
        SignalSema(mailbox->lock);
        return -1;
    }

    mailbox->messages[mailbox->write_index] =
        (u32) (uintptr_t)message;

    mailbox->write_index =
        (mailbox->write_index +1) % MAILBOX_MESSAGES;

    mailbox->count++;

    if (log_count < 16) {
        PSB_LOG(
            MBX,
            "thread %d gozou %08x na caixa %d fila %d",
            psbridge_psp_thread_uid(),
            (u32)(uintptr_t)message,
            uid,
            mailbox->count
        );
        log_count++;
    }

    result = SignalSema(mailbox->lock);

    if (result < 0)
        return result;

    result = SignalSema(mailbox->semaphore);
    return result < 0 ? result : 0;
}

static u64 mailbox_time(void)
{
    u32 seconds;
    u32 microseconds;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
    return (u64)seconds * 1000000 + microseconds;
}

int psbridge_receive_mailbox(int uid, void *output, void *timeout)
{
    PspMailbox *mailbox = find_mailbox(uid);
    u32 *destination = psbridge_game_pointer(output);
    u32 *remaining = psbridge_game_pointer(timeout);
    static u32 log_count;
    int result;

    if (!mailbox || !destination || (timeout && !remaining))
        return -1;

    if (!timeout) {
        result = WaitSema(mailbox->semaphore);
    } else {
        u32 limit = *remaining;
        u64 started = mailbox_time();

        for (;;) {
            u64 elapsed;
            u32 delay;

            result = PollSema(mailbox->semaphore);

            if (result >= 0)
                break;

            elapsed = mailbox_time() - started;

            if (elapsed >= limit)
                break;

            delay = limit - (u32)elapsed;

            if (delay > 1000)
                delay = 1000;

            DelayThread(delay);
        }

        {
            u64 elapsed = mailbox_time() - started;
            *remaining = elapsed >= limit ? 0 : limit - (u32)elapsed;
        }
    }

    if (result < 0) {
        return -1;
    }

    result = WaitSema(mailbox->lock);

    if (result < 0)
        return result;

    if (mailbox->count <= 0) {
        SignalSema(mailbox->lock);
        return -1;
    }

    *destination = mailbox->messages[mailbox->read_index];

    if (log_count < 16) {
        u32 *message = psbridge_game_pointer(
            (void *)(uintptr_t)*destination
        );
        u32 *work = message ? psbridge_game_pointer(
            (void *)(uintptr_t)message[2]
        ) : NULL;

        PSB_LOG(
            MBX,
            "thread %d lambeu a propria caceta e pegou %08x da caixa %d bagulho %08x %08x %08x %08x",
            psbridge_psp_thread_uid(),
            *destination,
            uid,
            message ? message[0] : 0,
            message ? message[1] : 0,
            message ? message[2] : 0,
            message ? message[3] : 0
        );

        PSB_LOG(
            MBX,
            "mulher dj abriu o trampo %08x e achou %08x %08x %08x %08x %08x %08x %08x %08x",
            message ? message[2] : 0,
            work ? work[0] : 0,
            work ? work[1] : 0,
            work ? work[2] : 0,
            work ? work[3] : 0,
            work ? work[4] : 0,
            work ? work[5] : 0,
            work ? work[6] : 0,
            work ? work[7] : 0
        );
        log_count++;
    }

    mailbox->read_index =
        (mailbox->read_index + 1) % MAILBOX_MESSAGES;

    mailbox->count--;

    SignalSema(mailbox->lock);

    return 0;
}

void *psbridge_thread_kernel_work(void)
{
    extern u32 main_kernel_work[];
    PspThread *thread = find_thread_by_host_id(GetThreadId());

    return thread ? thread->kernel_work : main_kernel_work;
}

int psbridge_psp_thread_uid(void)
{
    PspThread *thread = find_thread_by_host_id(GetThreadId());

    return thread ? thread->uid : 1;
}

int psbridge_delete_mailbox(int uid)
{
    PspMailbox *mailbox = find_mailbox(uid);

    if (!mailbox)
        return -1;

    DeleteSema(mailbox->semaphore);
    DeleteSema(mailbox->lock);

    memset(mailbox, 0, sizeof(*mailbox));

    return 0;
}

int psbridge_change_thread_priority(int uid, int priority)
{
    PspThread *thread = find_thread(uid);
    if (priority < 2 || priority > 126)
        return -1;
    if (uid == 0)
        return ChangeThreadPriority(GetThreadId(), priority);
    if (!thread)
        return -1;
    return ChangeThreadPriority(thread->thread_id, priority);
}

int psbridge_current_thread_priority(void)
{
    ee_thread_status_t status;
    int result = ReferThreadStatus(GetThreadId(), &status);

    return result < 0 ? result : status.current_priority;
}

int psbridge_refer_thread_status(int uid, void *output)
{
    int current = GetThreadId();
    PspThread *thread = uid ? find_thread(uid) : find_thread_by_host_id(current);
    u32 *data = output ? psbridge_game_pointer(output) : NULL;
    PspThreadStatus info = {0};
    ee_thread_status_t host;
    u32 size;

    if (uid && !thread && (uid != 1 || find_thread_by_host_id(current)))
        return (int)0x80020198u;
    if (!data)
        return (int)0x800200d3u;
    size = *data;
    if (size > sizeof(info))
        return (int)0x800201bcu;
    if (ReferThreadStatus(thread ? thread->thread_id : current, &host) < 0)
        return (int)0x80020198u;

    info.size = size > 104 ? sizeof(info) : 104;
    info.status = host.status;
    info.gp = guest_info.gp;
    info.initial_priority = host.initial_priority;
    info.current_priority = host.current_priority;
    info.wakeup_count = host.wakeupCount;
    info.exit_status = (int)0x800201a4u;
    if (thread) {
        memcpy(info.name, thread->name, sizeof(info.name));
        info.attributes = thread->attributes;
        info.entry = (u32)(uintptr_t)thread->entry;
        info.stack = (u32)(uintptr_t)thread->stack_buffer;
        info.stack_size = thread->stack_size;
    }

    if (host.status & THS_WAIT) {
        PspThreadWait wait = thread ? thread->wait : main_thread_wait;

        if (wait.type) {
            info.wait_type = wait.type;
            info.wait_uid = wait.uid;
        } else if (host.waitType == TSW_SLEEP) {
            info.wait_type = 1;
        }
    }

    memcpy(data, &info, size);
    return 0;
}

void psbridge_exit_guest_thread(int status)
{
    int index;
    int caller = GetThreadId();
    (void)status;
    for (index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used > 0 && threads[index].thread_id == caller) {
            static unsigned logs;
            if (logs++ < 32)
                PSB_LOG(THREAD, "thread %d vazou da putaria resultado %d", threads[index].uid, status);
            finish_guest_thread(threads[index].uid, status);
            threads[index].used = 0;
            break;
        }
    }
    ExitDeleteThread();
}

int psbridge_wakeup_guest_thread(int uid)
{
    PspThread *thread = find_thread(uid);
    if (!thread)
        return -1;
    return WakeupThread(thread->thread_id);
}

int psbridge_delete_guest_thread(int uid, int terminate)
{
    PspThread *thread = find_thread(uid);
    int result;
    if (!thread)
        return (int)0x80020198u;
    if (thread->thread_id == GetThreadId())
        return (int)0x80020197u;
    if (terminate) {
        ee_thread_status_t status;

        result = ReferThreadStatus(thread->thread_id, &status);
        if (result < 0)
            return result;
        if (status.status != THS_DORMANT) {
            result = TerminateThread(thread->thread_id);
            if (result < 0)
                return result;
        }
    }
    result = DeleteThread(thread->thread_id);
    if (result < 0)
        return result;
    if (terminate)
        finish_guest_thread(uid, (int)0x800201acu);
    thread->used = 0;
    return 0;
}
