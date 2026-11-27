#include <string.h>
#include <math.h>

#include "ge_internal.h"

enum {
    GE_VERTEX_FLOAT_UV_RGBA8888_BYTE_NORMAL_FLOAT_POSITION = 0x0001bf,
    GE_VERTEX_INDEXED_THROUGH_FLOAT = 0x80119f,
    GE_VERTEX_FLOAT_POSITION = 0x180,
    GE_VERTEX_U16_UV = 2,
    GE_VERTEX_BYTE_NORMAL = 1 << 5,
    GE_VERTEX_BYTE_WEIGHTS = 1 << 9,
    GE_VERTEX_WEIGHT_COUNT = 7 << 14,
};

typedef struct {
    u32 type;
    VertexLayout layout;
} GeVertexLayoutEntry;

typedef struct {
    u32 uv[2];
    u32 position[3];
} GeVertexSource;

typedef struct {
    u32 generation;
    GeVertexSource source;
    GeVertex vertex;
} GeVertexCacheEntry;

static GeVertexCacheEntry vertex_cache[512];
static u32 vertex_generation = 1;
static u32 transform_revision;

static int align_value(int value, int alignment)
{
    if (alignment <= 1)
        return value;

    return (value + alignment - 1) & ~(alignment - 1);
}

static VertexLayout vertex_layout(u32 type)
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

void ge_vertex_reader_init(GeVertexReader *reader, u32 type)
{
    static GeVertexLayoutEntry layouts[32];
    unsigned slot = (type ^ (type >> 5) ^ (type >> 12)) & 31;
    GeVertexLayoutEntry *entry = &layouts[slot];

    if (!entry->layout.stride || entry->type != type) {
        entry->type = type;
        entry->layout = vertex_layout(type);
    }

    reader->layout = entry->layout;
    reader->vertices = ge_pointer(ge_state.vertex_address);
    reader->type = type;
    reader->material = ge_state.command[GE_MATERIALAMBIENT] |
        ((ge_state.command[GE_MATERIALALPHA] & 0xff) << 24);
    reader->index_type = (type >> 11) & 3;
    reader->indices = reader->index_type ? ge_pointer(ge_state.index_address) : NULL;
    reader->indices_synchronized = 0;
    reader->vertices_synchronized = 0;

    if (type == GE_VERTEX_FLOAT_UV_RGBA8888_BYTE_NORMAL_FLOAT_POSITION) {
        u32 revision = ge3d_transform_revision();
        if (revision != transform_revision) {
            transform_revision = revision;
            if (!++vertex_generation) {
                memset(vertex_cache, 0, sizeof(vertex_cache));
                vertex_generation = 1;
            }
        }
    }
}

static u32 vertex_index(const GeVertexReader *reader, int number)
{
    if (!reader->indices)
        return number;

    int index_type = reader->index_type;
    unsigned index_size = 1u << (index_type - 1);
    const u8 *entry = reader->indices + (unsigned)number * index_size;
    if (!reader->indices_synchronized)
        ge_hardware_read(entry, index_size);
    if (index_type == 1)
        return *entry;

    if (index_type == 2)
        return *(const u16 *)entry;

    return *(const u32 *)entry & 0xffff;
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

static u32 vertex_primary_color(u32 color, u32 material, u32 type)
{
    const u32 *commands = ge_state.command;
    if ((type & (1u << 23)) || !(commands[GE_LIGHTING_ENABLE] & 1) ||
        ((commands[GE_LIGHT_ENABLE0] | commands[GE_LIGHT_ENABLE1] |
          commands[GE_LIGHT_ENABLE2] | commands[GE_LIGHT_ENABLE3]) & 1))
        return color;

    if (!(commands[GE_MATERIALUPDATE] & 1) || !(type & (7u << 2)))
        color = material;

    u32 ambient = commands[GE_AMBIENTCOLOR] |
        ((commands[GE_AMBIENTALPHA] & 0xff) << 24);
    u32 emissive = commands[GE_MATERIALEMISSIVE];
    if (ambient == 0xffffffffu && !emissive)
        return color;

    u32 result = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        u32 a = (color >> shift) & 0xff;
        u32 b = (ambient >> shift) & 0xff;
        u32 lit = ((2 * a + 1) * (2 * b + 1) >> 10) +
            ((emissive >> shift) & 0xff);
        result |= (lit > 255 ? 255 : lit) << shift;
    }

    return result;
}

static GeVertex read_single_vertex(const u8 *data, u32 type, const VertexLayout *layout,
    u32 material)
{
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int position = (type >> 7) & 3;
    int weight_type = (type >> 9) & 3;
    int weight_count = weight_type ? ((type >> 14) & 7) + 1 : 0;
    float weights[8];
    GeVertex vertex;

    vertex.x = 0.0f + vertex_component(data, layout->position, position, 0);
    vertex.y = 0.0f + vertex_component(data, layout->position, position, 1);
    if (position == 2 && (type & (1u << 23)))
        vertex.z = (float)*(const u16 *)(data + layout->position + 4);
    else
        vertex.z = 0.0f + vertex_component(data, layout->position, position, 2);

    if (texture) {
        vertex.u = 0.0f + vertex_texture(data, layout->texture, texture, 0);
        vertex.v = 0.0f + vertex_texture(data, layout->texture, texture, 1);
        ge_state.texture_u = vertex.u;
        ge_state.texture_v = vertex.v;
    } else {
        vertex.u = ge_state.texture_u;
        vertex.v = ge_state.texture_v;
    }

    u32 primary_color = color ? vertex_color(data, layout->color, color) : material;
    vertex.color = vertex_primary_color(primary_color, material, type);
    vertex.inv_w = 0.0f;
    for (int i = 0; i < weight_count; i++)
        weights[i] = vertex_weight(data, layout->weight, weight_type, i);

    ge3d_transform_vertex(&vertex, type, weights, weight_count);
    return vertex;
}

static void read_through_float_vertex(const u8 *data, GeVertex *vertex)
{
    float uv[2];
    float position[3];
    u32 color;

    if (((uintptr_t)data & 3) == 0)
        data = __builtin_assume_aligned(data, 4);

    memcpy(uv, data, sizeof(uv));
    memcpy(&color, data + 8, sizeof(color));
    memcpy(position, data + 12, sizeof(position));

    ge_state.texture_u = 0.0f + uv[0];
    ge_state.texture_v = 0.0f + uv[1];
    float x = 0.0f + position[0];
    float y = 0.0f + position[1];
    float z = 0.0f + position[2];

    *vertex = (GeVertex){
        .x = x, .y = y, .z = z,
        .u = ge_state.texture_u, .v = ge_state.texture_v,
        .color = color,
        .clip_x = x, .clip_y = y, .clip_z = z, .clip_w = 1.0f, .inv_w = 1.0f,
    };
}

static void read_float_position(const u8 *data, const GeVertexReader *reader, GeVertex *vertex)
{
    const VertexLayout *layout = &reader->layout;
    u32 type = reader->type;
    float position[3];

    if (((uintptr_t)data & 3) == 0)
        data = __builtin_assume_aligned(data, 4);

    memcpy(position, data + layout->position, sizeof(position));
    if (type & GE_VERTEX_U16_UV) {
        u16 uv[2];
        memcpy(uv, data + layout->texture, sizeof(uv));
        ge_state.texture_u = (float)uv[0];
        ge_state.texture_v = (float)uv[1];
    }

    *vertex = (GeVertex){
        .x = 0.0f + position[0], .y = 0.0f + position[1], .z = 0.0f + position[2],
        .u = ge_state.texture_u, .v = ge_state.texture_v,
        .color = vertex_primary_color(reader->material, reader->material, type),
    };

    float weights[8];
    int weight_count = 0;
    if (type & GE_VERTEX_BYTE_WEIGHTS) {
        weight_count = ((type >> 14) & 7) + 1;
        const u8 *source = data + layout->weight;
        for (int i = 0; i < weight_count; i++)
            weights[i] = (float)source[i] * (1.0f / 128.0f);
    }

    ge3d_transform_vertex(vertex, type, weights, weight_count);
}

static __attribute__((noinline)) GeVertex read_morphed_vertex(
    const u8 *data, u32 type, const VertexLayout *layout, u32 material)
{
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int position = (type >> 7) & 3;
    int weight_type = (type >> 9) & 3;
    int weight_count = weight_type ? ((type >> 14) & 7) + 1 : 0;
    int morph_count = ((type >> 18) & 7) + 1;
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

    vertex.color = material;
    vertex.inv_w = 0.0f;
    if (!texture) {
        uv_u = ge_state.texture_u;
        uv_v = ge_state.texture_v;
    }

    for (target = 0; target < morph_count; target++) {
        const u8 *morph_data = data + target * layout->target_stride;
        float blend = ge3d_morph_weight(target);
        float px;
        float py;
        float pz;
        if (blend == 0.0f)
            continue;

        px = vertex_component(morph_data, layout->position, position, 0);
        py = vertex_component(morph_data, layout->position, position, 1);
        pz = vertex_component(morph_data, layout->position, position, 2);
        if (position == 2 && (type & (1u << 23)))
            pz = (float)*(const u16 *)(morph_data + layout->position + 4);
        raw_x += blend * px;
        raw_y += blend * py;
        raw_z += blend * pz;

        if (texture) {
            uv_u += blend * vertex_texture(morph_data, layout->texture, texture, 0);
            uv_v += blend * vertex_texture(morph_data, layout->texture, texture, 1);
        }

        if (color) {
            u32 col = vertex_color(morph_data, layout->color, color);
            for (i = 0; i < 4; i++)
                color_values[i] += blend * (float)((col >> (i * 8)) & 255u);
        }

        for (i = 0; i < weight_count && i < 8; i++)
            weights[i] += blend * vertex_weight(morph_data, layout->weight, weight_type, i);
    }

    vertex.x = raw_x;
    vertex.y = raw_y;
    vertex.z = raw_z;
    vertex.u = uv_u;
    vertex.v = uv_v;
    if (color)
        vertex.color = vertex_mix_color(color_values);
    vertex.color = vertex_primary_color(vertex.color, material, type);
    if (texture) {
        ge_state.texture_u = vertex.u;
        ge_state.texture_v = vertex.v;
    }

    ge3d_transform_vertex(&vertex, type, weights, weight_count);

    return vertex;
}

static void read_float_vertex(const u8 *data, const VertexLayout *layout,
    u32 material, GeVertex *vertex)
{
    GeVertexSource source;
    u32 color;
    if (((uintptr_t)data & 3) == 0) {
        const u8 *aligned = __builtin_assume_aligned(data, 4);
        memcpy(source.uv, aligned, sizeof(source.uv));
        memcpy(source.position, aligned + 16, sizeof(source.position));
        memcpy(&color, aligned + 8, sizeof(color));
    } else {
        memcpy(source.uv, data + layout->texture, sizeof(source.uv));
        memcpy(source.position, data + layout->position, sizeof(source.position));
        memcpy(&color, data + layout->color, sizeof(color));
    }
    color = vertex_primary_color(color, material, GE_VERTEX_FLOAT_UV_RGBA8888_BYTE_NORMAL_FLOAT_POSITION);

    float u;
    float v;
    memcpy(&u, &source.uv[0], sizeof(u));
    memcpy(&v, &source.uv[1], sizeof(v));
    ge_state.texture_u = 0.0f + u;
    ge_state.texture_v = 0.0f + v;

    u32 hash = source.position[0] ^ (source.position[1] + 3 * source.position[2]) ^
        (source.uv[0] + 7 * source.uv[1]);
    hash ^= hash >> 18;
    hash ^= hash >> 9;
    GeVertexCacheEntry *entry = &vertex_cache[hash & 511];
    const GeVertexSource *cached = &entry->source;
    if (entry->generation == vertex_generation &&
        cached->position[0] == source.position[0] && cached->position[1] == source.position[1] &&
        cached->position[2] == source.position[2] && cached->uv[0] == source.uv[0] &&
        cached->uv[1] == source.uv[1]) {
        *vertex = entry->vertex;
        vertex->color = color;
        return;
    }

    float position[3];
    memcpy(position, source.position, sizeof(position));
    *vertex = (GeVertex){
        .x = 0.0f + position[0], .y = 0.0f + position[1], .z = 0.0f + position[2],
        .u = ge_state.texture_u, .v = ge_state.texture_v,
        .color = color,
    };
    ge3d_transform_vertex(vertex, GE_VERTEX_FLOAT_UV_RGBA8888_BYTE_NORMAL_FLOAT_POSITION, NULL, 0);
    entry->generation = vertex_generation;
    entry->source = source;
    entry->vertex = *vertex;
}

void read_vertex(const GeVertexReader *reader, int number, GeVertex *vertex)
{
    u32 index = vertex_index(reader, number);
    const u8 *data = reader->vertices;
    const VertexLayout *layout = &reader->layout;
    u32 type = reader->type;
    u32 material = reader->material;

    if (!data || layout->stride <= 0 || layout->target_stride <= 0) {
        *vertex = (GeVertex){ .color = material, .clip_flags = GE_CLIP_OUTSIDE_SCREEN };
        return;
    }

    data += index * (u32)layout->stride;
    if (!reader->vertices_synchronized)
        ge_hardware_read(data, layout->stride);

    if (type == GE_VERTEX_FLOAT_UV_RGBA8888_BYTE_NORMAL_FLOAT_POSITION) {
        read_float_vertex(data, layout, material, vertex);
    } else if (type == GE_VERTEX_INDEXED_THROUGH_FLOAT) {
        read_through_float_vertex(data, vertex);
    } else if ((type & ~(GE_VERTEX_U16_UV | GE_VERTEX_BYTE_NORMAL |
        GE_VERTEX_BYTE_WEIGHTS | GE_VERTEX_WEIGHT_COUNT)) == GE_VERTEX_FLOAT_POSITION) {
        read_float_position(data, reader, vertex);
    } else if (!(type & (7u << 18))) {
        *vertex = read_single_vertex(data, type, layout, material);
    } else {
        *vertex = read_morphed_vertex(data, type, layout, material);
    }
}
