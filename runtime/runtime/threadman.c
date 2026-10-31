#include <delaythread.h>
#include <kernel.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define THREAD_ARG_SIZE 256
#define THREAD_LIMIT 8
#define THREAD_STACK_MIN 0x8000
#define THREAD_STACK_MAX 0x100000
#define KERNEL_STACK_SIZE 0x1000
#define MAILBOX_LIMIT 8
#define MAILBOX_MESSAGES 32

typedef struct {
    int used;
    int uid;
    int thread_id;
    void *entry;
    u32 arg_size;
    void *arg;
    unsigned char arg_copy[THREAD_ARG_SIZE];
    void *kernel_work;
    void *stack_buffer;
    u32 stack_capacity;
    u32 stack_size;
} PspThread;

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
                    PRINT_LOG("thread esperou a putaria uid %d retorno %d tempo %u\n",
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
            PRINT_LOG("thread ainda ta esperando a putaria uid %d ee %d tempo %u\n",
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
        if (threads[index].used && threads[index].uid == uid)
            return &threads[index];
    }
    return NULL;
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
            PRINT_LOG("thread %d terminou a putaria retorno %d\n", thread->uid, result);
        finish_guest_thread(thread->uid, result);
    }

    thread->used = 0;
    ExitDeleteThread();

}

int psbridge_create_thread(const char *name, void *entry, int priority, int stack_size, u32 attributes, void *options)
{
    int index;
    (void)attributes;
    (void)options;

    for (index = 0; index < THREAD_LIMIT; index++) {
        ee_thread_t config;
        int thread_id;
        u32 needed;
        void *stack;

        if (threads[index].used)
            continue;

        if (stack_size <= 0)
            stack_size = THREAD_STACK_MIN;

        needed = ((u32)stack_size + 15) & ~15u;
        if (needed < THREAD_STACK_MIN)
            needed = THREAD_STACK_MIN;
        if (needed > THREAD_STACK_MAX)
            return -1;

        if (threads[index].stack_capacity < needed + KERNEL_STACK_SIZE) {
            stack = memalign(16, needed + KERNEL_STACK_SIZE);
            if (!stack) {
                PSB_LOG(THREAD, "pilha da thread peidou tamanho %u", needed);
                return -1;
            }
            free(threads[index].stack_buffer);
            threads[index].stack_buffer = stack;
            threads[index].stack_capacity = needed + KERNEL_STACK_SIZE;
        }

        if (priority < 2)
            priority = 2;

        if (priority > 126)
            priority = 126;

        memset(&config, 0, sizeof(config));

        config.func = run_thread;
        config.stack = threads[index].stack_buffer;
        config.stack_size = needed;
        config.gp_reg = &_gp;
        config.initial_priority = priority;

        thread_id = CreateThread(&config);

        if (thread_id < 0)
            return thread_id;

        threads[index].used = 1;
        threads[index].uid = next_thread_uid++;
        threads[index].thread_id = thread_id;
        threads[index].entry = entry;
        threads[index].stack_size = needed;
        threads[index].kernel_work =
            (u8 *)threads[index].stack_buffer + needed;

        PSB_LOG(
            THREAD,
            "thread %d nasceu na putaria entrada %08x pilha pedida %d pilha dada %d",
            threads[index].uid,
            (u32)(uintptr_t)entry,
            stack_size,
            needed
        );

        return threads[index].uid;
    }

    return -1;
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
    int current = GetThreadId();
    int index;

    for (index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used && threads[index].thread_id == current)
            return threads[index].kernel_work;
    }

    return main_kernel_work;
}

int psbridge_psp_thread_uid(void)
{
    int current = GetThreadId();
    int index;

    for (index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used && threads[index].thread_id == current)
            return threads[index].uid;
    }

    return 1;
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

void psbridge_exit_guest_thread(int status)
{
    int index;
    int caller = GetThreadId();
    (void)status;
    for (index = 0; index < THREAD_LIMIT; index++) {
        if (threads[index].used && threads[index].thread_id == caller) {
            static unsigned logs;
            if (logs++ < 32)
                PRINT_LOG("thread %d vazou da putaria resultado %d\n", threads[index].uid, status);
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

int psbridge_delete_guest_thread(int uid)
{
    PspThread *thread = find_thread(uid);
    int result;
    if (!thread)
        return -1;
    result = DeleteThread(thread->thread_id);
    if (result >= 0)
        memset(thread, 0, sizeof(*thread));
    return result;
}
