#include <string.h>

#include "ge_internal.h"

enum {
    GE_COMMAND_SPAN_BYTES = 256,
    GE_COMMAND_DRAW_CONFIG = 1,
    GE_COMMAND_TEXTURE_KEY = 2,
    GE_COMMAND_TRANSFORM = 4,
};

static const u8 command_flags[256] = {
    [0x2a ... GE_OFFSETY] = GE_COMMAND_TRANSFORM,
    [GE_VADDR_BASE] = GE_COMMAND_DRAW_CONFIG,
    [GE_VADDR_WIDTH] = GE_COMMAND_DRAW_CONFIG,
    [GE_FRAMEBUF_FORMAT] = GE_COMMAND_DRAW_CONFIG,
    [GE_DEPTHBUF_BASE] = GE_COMMAND_DRAW_CONFIG,
    [GE_DEPTHBUF_WIDTH] = GE_COMMAND_DRAW_CONFIG,
    [GE_DEPTHTEST] = GE_COMMAND_DRAW_CONFIG,
    [GE_DEPTH_TEST_ENABLE] = GE_COMMAND_DRAW_CONFIG,
    [GE_DEPTH_WRITE_DISABLE] = GE_COMMAND_DRAW_CONFIG,
    [GE_ALPHA_TEST_ENABLE] = GE_COMMAND_DRAW_CONFIG,
    [GE_ALPHATEST] = GE_COMMAND_DRAW_CONFIG,
    [GE_BLEND_ENABLE] = GE_COMMAND_DRAW_CONFIG,
    [GE_BLENDMODE] = GE_COMMAND_DRAW_CONFIG,
    [GE_BLENDFIXEDB] = GE_COMMAND_DRAW_CONFIG,
    [GE_TEXTURE_ENABLE] = GE_COMMAND_DRAW_CONFIG,
    [GE_TEXFORMAT] = GE_COMMAND_DRAW_CONFIG | GE_COMMAND_TEXTURE_KEY,
    [GE_TEXFUNC] = GE_COMMAND_DRAW_CONFIG | GE_COMMAND_TEXTURE_KEY,
    [GE_SCISSOR1] = GE_COMMAND_DRAW_CONFIG,
    [GE_SCISSOR2] = GE_COMMAND_DRAW_CONFIG,
    [GE_CLEARMODE] = GE_COMMAND_DRAW_CONFIG,
    [GE_TEXADDR0] = GE_COMMAND_TEXTURE_KEY,
    [GE_TEXBUFWIDTH0] = GE_COMMAND_TEXTURE_KEY,
    [GE_TEXSIZE0] = GE_COMMAND_TEXTURE_KEY | GE_COMMAND_TRANSFORM,
    [GE_TEXMODE] = GE_COMMAND_TEXTURE_KEY,
    [GE_CLUTFORMAT] = GE_COMMAND_TEXTURE_KEY,
    [GE_TEXENVCOLOR] = GE_COMMAND_TEXTURE_KEY,
};

static int next_list_id = 1;
static int next_callback_id = 1;
static GeCallback ge_callbacks[16];
static u32 diag_ge_callbacks;

GeCallback *find_ge_callback(int id)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (ge_callbacks[i].used && ge_callbacks[i].id == id)
            return &ge_callbacks[i];
    }

    return NULL;
}

int register_ge_callback(u32 address)
{
    const u32 *data = psbridge_game_pointer((const void *)(uintptr_t)address);
    int i;
    if (!data)
        return -1;

    for (i = 0; i < 16; i++) {
        if (!ge_callbacks[i].used) {
            GeCallback *cb = &ge_callbacks[i];
            cb->used = 1;
            cb->id = next_callback_id++;
            cb->signal_function = data[0];
            cb->signal_arg = data[1];
            cb->finish_function = data[2];
            cb->finish_arg = data[3];
            PSB_LOG(GE, "ge cadastrou callback safada id %d sinal %08x fim %08x",
                cb->id, cb->signal_function, cb->finish_function);
            return cb->id;
        }
    }

    return -1;
}

static void fire_ge_callback(int callback_id, int finish, u32 data)
{
    GeCallback *cb = find_ge_callback(callback_id);
    u32 function;
    u32 argument;
    if (!cb)
        return;

    function = finish ? cb->finish_function : cb->signal_function;
    argument = finish ? cb->finish_arg : cb->signal_arg;
    if (!function)
        return;

    diag_ge_callbacks++;
    if (diag_ge_callbacks <= 16 || (diag_ge_callbacks % 128 == 0))
        PSB_LOG(GE, "ge callback %s gozou id %d dado %08x endereco %08x vezes %u",
            finish ? "fim" : "sinal", callback_id, data, function, diag_ge_callbacks);
    psbridge_call_callback((void *)(uintptr_t)function, data, argument, 0);
}

void *ge_pointer(u32 address)
{
    u32 video = (u32)(uintptr_t)psbridge_video_memory();

    address &= 0x3fffffff;

    if (address >= 0x04000000 && address < 0x04200000)
        return (u8 *)psbridge_video_memory() + address - 0x04000000;

    if (address < 0x00200000)
        return (u8 *)psbridge_video_memory() + address;

    if (address >= video && address < video + 0x00200000)
        return (void *)(uintptr_t)address;

    {
        void *result = psbridge_game_pointer((void *)(uintptr_t)address);
        if ((u32)(uintptr_t)result == address && address >= 0x00200000 && address < 0x02000000)
            return (void *)(uintptr_t)(address | 0x80000000);

        return result;
    }
}

u32 relative_address(u32 value)
{
    u32 base = (ge_state.command[GE_BASE] & 0x000f0000) << 8;

    return (ge_state.offset_address + base + (value & 0x00ffffff)) & 0x0fffffff;
}

u32 buffer_address(int address_command, int width_command)
{
    return
        (ge_state.command[address_command] & 0x00fffff0) |
        ((ge_state.command[width_command] << 8) & 0x0f000000);
}

static void transfer_pixels(void)
{
    int source_width = ge_state.command[GE_TRANSFERSRCW] & 0x7f8;
    int destination_width = ge_state.command[GE_TRANSFERDSTW] & 0x7f8;
    int source_x = ge_state.command[GE_TRANSFERSRCPOS] & 0x3ff;
    int source_y = (ge_state.command[GE_TRANSFERSRCPOS] >> 10) & 0x3ff;
    int destination_x = ge_state.command[GE_TRANSFERDSTPOS] & 0x3ff;
    int destination_y = (ge_state.command[GE_TRANSFERDSTPOS] >> 10) & 0x3ff;
    int width = (ge_state.command[GE_TRANSFERSIZE] & 0x3ff) + 1;
    int height = ((ge_state.command[GE_TRANSFERSIZE] >> 10) & 0x3ff) + 1;
    int bytes = (ge_state.command[GE_TRANSFERSTART] & 1) ? 4 : 2;
    const u8 *source = ge_pointer(buffer_address(GE_TRANSFERSRC, GE_TRANSFERSRCW));
    u8 *destination = ge_pointer(buffer_address(GE_TRANSFERDST, GE_TRANSFERDSTW));
    int y;

    if (!source || !destination || !source_width || !destination_width)
        return;

    for (y = 0; y < height; y++) {
        memcpy(
            destination + ((destination_y + y) * destination_width + destination_x) * bytes,
            source + ((source_y + y) * source_width + source_x) * bytes,
            width * bytes
        );
    }
}

GeList *find_list(int id)
{
    int index;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (ge_lists[index].used && ge_lists[index].id == id)
            return &ge_lists[index];
    }

    return NULL;
}

void execute_list(GeList *list)
{
    int commands = 0;
    const u32 *ram_commands = NULL;
    u32 ram_start = 0;

    psbridge_graphics_lock();
    if (list->done) {
        psbridge_graphics_unlock();
        return;
    }

    ge_hardware_invalidate_surfaces();
    ge_hardware_texture_dirty();
    ge_dxt_new_list();

    while (!list->done && commands++ < GE_COMMAND_LIMIT) {
        u32 word;
        u32 command;
        u32 argument;

        if (list->stall && list->pc == list->stall)
            break;

        if (!list->pc || (list->pc & 3)) {
            list->done = 1;
            break;
        }

        u32 pc = list->pc & 0x3fffffffu;
        unsigned offset = pc - ram_start;
        if (!ram_commands || offset >= GE_COMMAND_SPAN_BYTES) {
            ram_commands = NULL;
            if (pc >= 0x08800000u && pc <= 0x0a000000u - GE_COMMAND_SPAN_BYTES) {
                ram_commands = psbridge_ram_pointer(pc, GE_COMMAND_SPAN_BYTES);
                ram_start = pc;
                offset = 0;
            }
        }

        if (ram_commands) {
            word = ram_commands[offset / sizeof(u32)];
        } else {
            const u32 *instruction = ge_pointer(list->pc);
            if (!instruction) {
                list->done = 1;
                break;
            }

            ge_hardware_read(instruction, sizeof(*instruction));
            word = *instruction;
        }

        command = word >> 24;
        argument = word & 0x00ffffff;
        list->pc += 4;
        unsigned flags = command_flags[command];
        if (ge_state.command[command] != argument) {
            if (flags & GE_COMMAND_TEXTURE_KEY)
                ge_hardware_texture_unbind();

            ge_hardware_config_dirty |= flags & GE_COMMAND_DRAW_CONFIG;
            ge_state.command[command] = argument;
        }
        if (flags & GE_COMMAND_TRANSFORM)
            ge3d_command(command, argument);

        switch (command) {
        case GE_TEXFUNC:
        case GE_VERTEXTYPE:
        case GE_TEXFORMAT: {
            static int ge_rgb01_logs = 0;

            if (ge_rgb01_logs < 18) {
                ge_rgb01_logs++;
                PSB_LOG(
                    GE,
                    "ge recebeu uma leitada %08x com a potencia de mil homens %06x",
                    (unsigned int)command,
                    (unsigned int)argument
                );
            }
            break;
        }
        case GE_BLENDMODE:
            if (!ge_blend_mode_seen) {
                ge_blend_mode_seen = 1;
                ge_hardware_config_dirty = 1;
            }
            break;
        case GE_VADDR:
            ge_state.vertex_address = relative_address(argument);
            break;
        case GE_IADDR:
            ge_state.index_address = relative_address(argument);
            break;
        case GE_OFFSETADDR:
            ge_state.offset_address = argument << 8;
            break;
        case GE_ORIGIN:
            ge_state.offset_address = list->pc - 4;
            break;
        case GE_PRIM:
            list->draws++;
            draw_primitive(argument);
            break;
        case GE_TRANSFERSTART:
            ge_hardware_texture_dirty();
            ge_hardware_cpu_write();
            transfer_pixels();
            ge_dxt_dirty();
            break;
        case GE_LOADCLUT:
            if (load_clut(argument))
                ge_hardware_texture_unbind();
            break;
        case 0xcb:
            ge_hardware_texture_dirty();
            ge_dxt_dirty();
            break;
        case GE_JUMP:
            list->pc = relative_address(argument & 0x00fffffc);
            break;
        case GE_CALL:
            if (list->stack_size < GE_STACK_LIMIT) {
                list->stack[list->stack_size] = list->pc;
                list->stack_offset[list->stack_size] = ge_state.offset_address;
                list->stack_size++;
                list->pc = relative_address(argument & 0x00fffffc);
            }
            break;
        case GE_RET:
            if (list->stack_size > 0) {
                list->stack_size--;
                list->pc = list->stack[list->stack_size];
                ge_state.offset_address = list->stack_offset[list->stack_size];
            }
            break;
        case GE_SIGNAL:
            list->signal_id = argument & 0xffff;
            list->pending_signal = 1;
            break;
        case GE_FINISH:
            list->finish_id = argument;
            list->pending_finish = 1;
            break;
        case GE_END: {
            ge_hardware_flush();
            list->done = 1;
            if (list->draws) {
                u32 target = buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH);

                if (target <= 0x200000u - 0x88000u &&
                    (ge_state.command[GE_FRAMEBUF_FORMAT] & 3u) == 3u) {
                    ge_ready_buffer = target;
                    ge_ready_sequence++;
                }
            }

            int callback = list->callback_id;
            int pending_signal = list->pending_signal;
            int pending_finish = list->pending_finish;
            u32 signal = list->signal_id;
            u32 finish = list->finish_id;
            list->pending_signal = 0;
            list->pending_finish = 0;
            psbridge_graphics_unlock();
            if (pending_signal)
                fire_ge_callback(callback, 0, signal);
            if (pending_finish)
                fire_ge_callback(callback, 1, finish);
            return;
        }
        }
    }

    ge_hardware_flush();
    if (commands >= GE_COMMAND_LIMIT) {
        list->done = 1;
    }
    psbridge_graphics_unlock();
}

int enqueue_list(u32 address, u32 stall, int callback_id)
{
    GeList *list = NULL;
    int index;

    address &= 0x3fffffff;
    stall &= 0x3fffffff;

    if (!address || (address & 3) || (stall & 3))
        return -1;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (!ge_lists[index].used || ge_lists[index].done) {
            list = &ge_lists[index];
            break;
        }
    }

    if (!list) {
        return -1;
    }

    memset(list, 0, sizeof(*list));
    list->used = 1;
    list->id = next_list_id++;
    list->pc = address;
    list->stall = stall;
    list->callback_id = callback_id;
    execute_list(list);
    return list->id;
}
