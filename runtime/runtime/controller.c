#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <delaythread.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"

typedef struct {
    u32 timestamp;
    u32 buttons;
    u8 lx;
    u8 ly;
    u8 reserved[6];
} PspControllerData;

#define CTRL_HISTORY_SIZE 64
#define CTRL_SAMPLE_INTERVAL_US 16683u

static PspControllerData controller_history[CTRL_HISTORY_SIZE];
static u64 controller_written;
static u64 controller_last_read;
static u64 controller_last_sample;
static int controller_history_ready;
static int controller_sampling_mode;

static unsigned char pad_buffer[256] __attribute__((aligned(64)));
static unsigned char controller_stack[8192] __attribute__((aligned(16)));
static volatile int controller_state;

static void controller_setup_thread(void *arg)
{
    int sio;
    int padman;
    int ready;
    (void)arg;

    sio = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    padman = SifLoadModule("rom0:PADMAN", 0, NULL);
    PRINT_LOG("controle botando o pad pra trabalhar sio %d pad %d\n", sio, padman);

    ready = padInit(0);
    if (ready > 0)
        ready = padPortOpen(0, 0, pad_buffer);

    if (ready > 0) {
        controller_state = 2;
        PRINT_LOG("controle acordou e ta no grau\n");
    } else {
        controller_state = -1;
        PRINT_LOG("controle broxou na inicializacao %d\n", ready);
    }

    ExitDeleteThread();
}

static void start_controller(void)
{
    ee_thread_t config;
    int interrupt_state;
    int thread_id;
    int start = 0;

    interrupt_state = DIntr();
    if (controller_state == 0) {
        controller_state = 1;
        start = 1;
    }
    if (interrupt_state != 0)
        EIntr();

    if (!start)
        return;

    memset(&config, 0, sizeof(config));
    config.func = controller_setup_thread;
    config.stack = controller_stack;
    config.stack_size = sizeof(controller_stack);
    config.gp_reg = &_gp;
    config.initial_priority = 35;

    thread_id = CreateThread(&config);
    if (thread_id < 0) {
        controller_state = -1;
        PRINT_LOG("controle peidou criando thread %d\n", thread_id);
        return;
    }

    if (StartThread(thread_id, NULL) < 0) {
        DeleteThread(thread_id);
        controller_state = -1;
        PRINT_LOG("controle peidou abrindo thread\n");
    }
}

static void read_controller(PspControllerData *sample)
{
    struct padButtonStatus pad;
    u32 pressed;
    int state;

    state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return;

    if (!padRead(0, 0, &pad))
        return;

    pressed = 0xffff ^ pad.btns;

    sample->buttons = pressed & 0xf0f9;

    if (pressed & (PAD_L1 | PAD_L2))
        sample->buttons |= 0x100;

    if (pressed & (PAD_R1 | PAD_R2))
        sample->buttons |= 0x200;

    sample->lx = pad.ljoy_h;
    sample->ly = pad.ljoy_v;
}

static u64 controller_now(void)
{
    u32 seconds;
    u32 micros;

    TimerBusClock2USec(GetTimerSystemTime(), &seconds, &micros);
    return (u64)seconds * 1000000 + micros;
}

static void controller_update_history(void)
{
    u64 now = controller_now();
    u64 steps;
    u64 index;
    PspControllerData current;

    if (!controller_history_ready) {
        memset(&current, 0, sizeof(current));
        current.lx = 128;
        current.ly = 128;
        current.timestamp = (u32)now;
        for (index = 0; index < CTRL_HISTORY_SIZE; index++)
            controller_history[index] = current;
        controller_written = CTRL_HISTORY_SIZE;
        controller_last_read = controller_written - 1;
        controller_last_sample = now;
        controller_history_ready = 1;
    }

    if (now <= controller_last_sample)
        return;

    steps = (now - controller_last_sample) / CTRL_SAMPLE_INTERVAL_US;
    if (!steps)
        return;

    if (steps > CTRL_HISTORY_SIZE) {
        steps = CTRL_HISTORY_SIZE;
        controller_last_sample = now - steps * CTRL_SAMPLE_INTERVAL_US;
    }

    current = controller_history[(controller_written - 1) % CTRL_HISTORY_SIZE];
    for (index = 0; index < steps; index++) {
        u32 previous = current.buttons;

        controller_last_sample += CTRL_SAMPLE_INTERVAL_US;
        if (index + 1 == steps) {
            start_controller();
            if (controller_state == 2)
                read_controller(&current);
        }

        current.timestamp = (u32)controller_last_sample;
        if (!controller_sampling_mode) {
            current.lx = 128;
            current.ly = 128;
        }

        if ((previous ^ current.buttons) & 0x4000u) {
            static unsigned cross_logs;
            if (cross_logs++ < 20)
                PRINT_LOG("controle X %s na putaria amostra %u botoes %08x\n",
                    (current.buttons & 0x4000u) ? "entrou" : "vazou",
                    current.timestamp, current.buttons);
        }

        if ((previous ^ current.buttons) & 0x8u) {
            static unsigned logs;
            if (logs++ < 16)
                PRINT_LOG("controle START %s na amostra %u botoes %08x\n",
                    (current.buttons & 0x8u) ? "entrou na putaria" : "vazou da putaria",
                    current.timestamp, current.buttons);
        }

        controller_history[controller_written % CTRL_HISTORY_SIZE] = current;
        controller_written++;
    }
}

int psbridge_controller_sampling(int mode)
{
    int previous = controller_sampling_mode;

    if (mode != 0 && mode != 1)
        return -1;

    controller_sampling_mode = mode;
    return previous;
}

int psbridge_controller_get_sampling(void *output)
{
    u32 *mode = psbridge_game_pointer(output);

    if (!mode)
        return -1;

    *mode = (u32)controller_sampling_mode;
    return 0;
}

int psbridge_controller_read(void *output, int count, u32 nid)
{
    PspControllerData *samples = psbridge_game_pointer(output);
    int peek = nid == 0x3a622550 || nid == 0xc152080a;
    int negative = nid == 0x60b81f86 || nid == 0xc152080a;
    u64 available;
    u64 start;
    int index;

    if (!samples || count <= 0 || count > CTRL_HISTORY_SIZE)
        return -1;

    start_controller();
    controller_update_history();

    if (!peek) {
        while (controller_last_read == controller_written) {
            DelayThread(1000);
            controller_update_history();
        }
        available = controller_written - controller_last_read;
        if (available > (u64)count)
            available = count;
        if (available > CTRL_HISTORY_SIZE)
            available = CTRL_HISTORY_SIZE;
    } else {
        available = count;
    }

    start = controller_written - available;
    for (index = 0; index < (int)available; index++) {
        samples[index] = controller_history[(start + index) % CTRL_HISTORY_SIZE];
        if (negative)
            samples[index].buttons = ~samples[index].buttons;
    }

    if (!peek)
        controller_last_read = controller_written;

    if (peek && available > 1 &&
        ((samples[0].buttons ^ samples[available - 1].buttons) & 0x8u)) {
        static unsigned logs;
        if (logs++ < 12)
            PRINT_LOG("controle entregou %u amostras START velho %08x novo %08x\n",
                (u32)available, samples[0].buttons, samples[available - 1].buttons);
    }

    return (int)available;
}
