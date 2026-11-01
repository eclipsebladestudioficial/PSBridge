#include <string.h>

#include "ge_internal.h"

static int align_value(int value, int alignment)
{
    if (alignment <= 1)
        return value;

    return (value + alignment - 1) & ~(alignment - 1);
}

VertexLayout vertex_layout(u32 type)
{
    static const int texture_size[] = {0, 2, 4, 8};
    static const int texture_align[] = {1, 1, 2, 4};
    static const int color_size[] = {0, 0, 0, 0, 2, 2, 2, 4};
    static const int color_align[] = {1, 1, 1, 1, 2, 2, 2, 4};
    static const int normal_size[] = {0, 3, 6, 12};
    static const int normal_align[] = {1, 1, 2, 4};
    static const int position_size[] = {3, 3, 6, 12};
    static const int position_align[] = {1, 1, 2, 4};
    static const int weight_size[] = {0, 1, 2, 4};
    static const int weight_align[] = {1, 1, 2, 4};
    VertexLayout layout;
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int normal = (type >> 5) & 3;
    int position = (type >> 7) & 3;
    int weight = (type >> 9) & 3;
    int weight_count = ((type >> 14) & 7) + 1;
    int morph_count = ((type >> 18) & 7) + 1;
    int size = 0;
    int biggest = 1;

    memset(&layout, 0, sizeof(layout));

    if (weight) {
        layout.weight = size;
        size += weight_size[weight] * weight_count;
        biggest = weight_align[weight];
    }

    if (texture) {
        size = align_value(size, texture_align[texture]);
        layout.texture = size;
        size += texture_size[texture];
        if (texture_align[texture] > biggest)
            biggest = texture_align[texture];
    } else {
        layout.texture = -1;
    }

    if (color) {
        size = align_value(size, color_align[color]);
        layout.color = size;
        size += color_size[color];
        if (color_align[color] > biggest)
            biggest = color_align[color];
    } else {
        layout.color = -1;
    }

    if (normal) {
        size = align_value(size, normal_align[normal]);
        layout.normal = size;
        size += normal_size[normal];
        if (normal_align[normal] > biggest)
            biggest = normal_align[normal];
    }

    size = align_value(size, position_align[position]);
    layout.position = size;
    size += position_size[position];

    if (position_align[position] > biggest)
        biggest = position_align[position];

    layout.stride = align_value(size, biggest) * morph_count;

    return layout;
}

static u32 vertex_index(int number, u32 type)
{
    int index_type = (type >> 11) & 3;
    const void *indices = ge_pointer(ge_state.index_address);

    if (!index_type || !indices)
        return number;

    if (index_type == 1)
        return ((const u8 *)indices)[number];

    if (index_type == 2)
        return ((const u16 *)indices)[number];

    return ((const u32 *)indices)[number] & 0xffff;
}

GeVertex read_vertex(int number, u32 type, VertexLayout layout)
{
    static int vertex_depth_logs;
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int position = (type >> 7) & 3;
    u32 index = vertex_index(number, type);
    const u8 *data = ge_pointer(ge_state.vertex_address);
    GeVertex vertex;
    float raw_x;
    float raw_y;
    float raw_z;

    memset(&vertex, 0, sizeof(vertex));
    vertex.color = 0xffffffff;

    if (!data)
        return vertex;

    data += index * layout.stride;

    if (texture == 1) {
        vertex.u = data[layout.texture];
        vertex.v = data[layout.texture + 1];
    } else if (texture == 2) {
        vertex.u = *(const u16 *)(data + layout.texture);
        vertex.v = *(const u16 *)(data + layout.texture + 2);
    } else if (texture == 3) {
        vertex.u = *(const float *)(data + layout.texture);
        vertex.v = *(const float *)(data + layout.texture + 4);
    } else {
        vertex.u = ge_state.texture_u;
        vertex.v = ge_state.texture_v;
    }

    if (texture) {
        ge_state.texture_u = vertex.u;
        ge_state.texture_v = vertex.v;
    }

    if (color >= 4 && color <= 6)
        vertex.color = unpack_color(*(const u16 *)(data + layout.color), color - 4);
    else if (color == 7)
        vertex.color = *(const u32 *)(data + layout.color);

    if (position == 1) {
        vertex.x = *(const s8 *)(data + layout.position);
        vertex.y = *(const s8 *)(data + layout.position + 1);
        vertex.z = *(const s8 *)(data + layout.position + 2);
    } else if (position == 2) {
        vertex.x = *(const s16 *)(data + layout.position);
        vertex.y = *(const s16 *)(data + layout.position + 2);
        vertex.z = *(const s16 *)(data + layout.position + 4);
    } else if (position == 3) {
        vertex.x = *(const float *)(data + layout.position);
        vertex.y = *(const float *)(data + layout.position + 4);
        vertex.z = *(const float *)(data + layout.position + 8);
    }

    raw_x = vertex.x;
    raw_y = vertex.y;
    raw_z = vertex.z;
    ge3d_transform_vertex(&vertex, type, ge_state.command);
    if (!(type & (1u << 23)) &&
        ((ge_state.command[GE_DEPTH_TEST_ENABLE] & 1) ||
        (type & 0x00000e60)) && vertex_depth_logs < 48) {
        vertex_depth_logs++;
        PRINT_3D_LOG("vertice do mapa meteu tipo %08x numero %d cru %.3f %.3f %.3f clip %.3f %.3f %.3f %.3f tela %.3f %.3f %.3f uv %.3f %.3f\n",
            type, number, raw_x, raw_y, raw_z,
            vertex.clip_x, vertex.clip_y, vertex.clip_z,
            vertex.clip_w, vertex.x, vertex.y, vertex.z, vertex.u, vertex.v);
    }
    return vertex;
}
