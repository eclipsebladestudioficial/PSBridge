#include <string.h>
#include <math.h>

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

    layout.target_stride = align_value(size, biggest);
    layout.stride = layout.target_stride * morph_count;

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

static float vertex_component(const u8 *data, int offset, int format, int component)
{
    if (format == 1)
        return (float)*(const s8 *)(data + offset + component);
    if (format == 2)
        return (float)*(const s16 *)(data + offset + 2 * component);
    if (format == 3)
        return *(const float *)(data + offset + 4 * component);
    return 0.0f;
}

static float vertex_weight(const u8 *data, int offset, int format, int component)
{
    if (format == 1)
        return (float)data[offset + component] * (1.0f / 128.0f);
    if (format == 2)
        return (float)*(const u16 *)(data + offset + 2 * component) * (1.0f / 32768.0f);
    if (format == 3)
        return *(const float *)(data + offset + 4 * component);
    return 0.0f;
}

static float vertex_texture(const u8 *data, int offset, int format, int component)
{
    if (format == 1)
        return (float)data[offset + component];
    if (format == 2)
        return (float)*(const u16 *)(data + offset + 2 * component);
    if (format == 3)
        return *(const float *)(data + offset + 4 * component);
    return 0.0f;
}

static u32 vertex_color(const u8 *data, int offset, int format)
{
    if (format >= 4 && format <= 6)
        return unpack_color(*(const u16 *)(data + offset), format - 4);
    if (format == 7)
        return *(const u32 *)(data + offset);
    return 0xffffffffu;
}

static u32 vertex_mix_color(const float values[4])
{
    u32 output = 0;
    int i;
    for (i = 0; i < 4; i++) {
        float component = values[i];
        if (!(component >= 0.0f))
            component = 0.0f;
        if (component > 255.0f)
            component = 255.0f;
        output |= (u32)component << (8 * i);
    }
    return output;
}

GeVertex read_vertex(int number, u32 type, VertexLayout layout)
{
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int position = (type >> 7) & 3;
    int weight_type = (type >> 9) & 3;
    int weight_count = weight_type ? ((type >> 14) & 7) + 1 : 0;
    int morph_count = ((type >> 18) & 7) + 1;
    u32 index = vertex_index(number, type);
    const u8 *data = ge_pointer(ge_state.vertex_address);
    GeVertex vertex;
    float weights[8] = {0};
    float color_values[4] = {0};
    float raw_x = 0.0f;
    float raw_y = 0.0f;
    float raw_z = 0.0f;
    float uv_u = 0.0f;
    float uv_v = 0.0f;
    int target;
    int i;

    memset(&vertex, 0, sizeof(vertex));
    vertex.color = 0xffffffffu;

    if (!data || layout.stride <= 0 || layout.target_stride <= 0) {
        return vertex;
    }

    data += index * (u32)layout.stride;

    if (!texture) {
        uv_u = ge_state.texture_u;
        uv_v = ge_state.texture_v;
    }

    for (target = 0; target < morph_count; target++) {
        const u8 *morph_data = data + target * layout.target_stride;
        float blend = morph_count == 1 ? 1.0f : ge3d_morph_weight(target);
        float px;
        float py;
        float pz;
        if (blend == 0.0f)
            continue;

        px = vertex_component(morph_data, layout.position, position, 0);
        py = vertex_component(morph_data, layout.position, position, 1);
        pz = vertex_component(morph_data, layout.position, position, 2);
        if (position == 2 && (type & (1u << 23)))
            pz = (float)*(const u16 *)(morph_data + layout.position + 4);
        raw_x += blend * px;
        raw_y += blend * py;
        raw_z += blend * pz;

        if (texture) {
            uv_u += blend * vertex_texture(morph_data, layout.texture, texture, 0);
            uv_v += blend * vertex_texture(morph_data, layout.texture, texture, 1);
        }

        if (color) {
            u32 col = vertex_color(morph_data, layout.color, color);
            for (i = 0; i < 4; i++)
                color_values[i] += blend * (float)((col >> (i * 8)) & 255u);
        }

        for (i = 0; i < weight_count && i < 8; i++)
            weights[i] += blend * vertex_weight(morph_data, layout.weight, weight_type, i);
    }

    vertex.x = raw_x;
    vertex.y = raw_y;
    vertex.z = raw_z;
    vertex.u = uv_u;
    vertex.v = uv_v;
    if (color)
        vertex.color = vertex_mix_color(color_values);
    if (texture) {
        ge_state.texture_u = vertex.u;
        ge_state.texture_v = vertex.v;
    }

    ge3d_transform_vertex(&vertex, type, ge_state.command, weights, weight_count);

    return vertex;
}
