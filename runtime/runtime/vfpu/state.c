#include "vfpu_internal.h"
#include <math.h>
#include <string.h>

u32 vfpu_vr[128];
u32 vfpu_ctrl[16];
u32 vfpu_prefix_s = 0x000000e4;
u32 vfpu_prefix_t = 0x000000e4;
u32 vfpu_prefix_d;
u8 vfpu_have_s;
u8 vfpu_have_t;
u8 vfpu_have_d;
u32 vfpu_executed;
u32 vfpu_unsupported;
u32 vfpu_invalid_memory;
u32 vfpu_last_unsupported;
u32 vfpu_last_unsupported_pc;
u32 vfpu_last_invalid_address;

float vfpu_as_float(u32 bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
u32 vfpu_as_bits(float f)
{
    u32 bits;
    memcpy(&bits, &f, 4);
    return bits;
}

int vfpu_vector_lane(int r, int n, int lane)
{
    int m = (r >> 2) & 7, col = r & 3;
    int row = n == 1 ? (r >> 5) & 3 : n == 3 ? (r >> 6) & 1 : (r >> 5) & 2;
    if (n != 1 && (r & 0x20))
        return m * 4 + ((row + lane) & 3) + col * 32;
    return m * 4 + col + ((row + lane) & 3) * 32;
}
int vfpu_matrix_lane(int r, int n, int column, int rowidx)
{
    int m = (r >> 2) & 7, col = r & 3;
    int row = n == 3 ? (r >> 6) & 1 : (r >> 5) & 2;
    if (r & 0x20)
        return m * 4 + ((row + rowidx) & 3) + ((col + column) & 3) * 32;
    return m * 4 + ((col + column) & 3) + ((row + rowidx) & 3) * 32;
}
void vfpu_read_vector(int r, int n, float out[4])
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = vfpu_as_float(vfpu_vr[vfpu_vector_lane(r, n, i)]);
}

void vfpu_read_prefix(int r, int n, float out[4], u32 p, int active)
{
    float original[4] = {0, 0, 0, 0};
    int i;
    vfpu_read_vector(r, n, original);
    if (!active)
    {
        memcpy(out, original, sizeof(original));
        return;
    }
    for (i = 0; i < n; i++)
    {
        int src = (p >> (i * 2)) & 3;
        float v = original[src];
        if (p & (1u << (12 + i)))
        {
            static const float constants[8] = {0.0f, 1.0f,        2.0f,  0.5f,
                                               3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};
            v = constants[src | ((p >> (8 + i)) & 1) * 4];
        }
        else if (p & (1u << (8 + i)))
            v = fabsf(v);
        if (p & (1u << (16 + i)))
            v = -v;
        out[i] = v;
    }
}
void vfpu_write_vector(int r, int n, const float in[4], int use_prefix)
{
    int i;
    for (i = 0; i < n; i++)
    {
        float v = in[i];
        if (use_prefix && vfpu_have_d)
        {
            int sat = (vfpu_prefix_d >> (i * 2)) & 3;
            if (sat == 1)
            {
                if (v < 0)
                    v = 0;
                if (v > 1)
                    v = 1;
            }
            if (sat == 3)
            {
                if (v < -1)
                    v = -1;
                if (v > 1)
                    v = 1;
            }
            if (vfpu_prefix_d & (1u << (8 + i)))
                continue;
        }
        vfpu_vr[vfpu_vector_lane(r, n, i)] = vfpu_as_bits(v);
    }
}

void vfpu_finish_prefix(void)
{
    vfpu_have_s = vfpu_have_t = vfpu_have_d = 0;
    vfpu_prefix_s = 0xe4u;
    vfpu_prefix_t = 0xe4u;
    vfpu_prefix_d = 0;
    vfpu_ctrl[0] = 0xe4u;
    vfpu_ctrl[1] = 0xe4u;
    vfpu_ctrl[2] = 0;
}

void vfpu_matrix_read(int r, int n, float mat[4][4])
{
    int j, k;
    for (j = 0; j < n; j++)
        for (k = 0; k < n; k++)
            mat[j][k] = vfpu_as_float(vfpu_vr[vfpu_matrix_lane(r, n, j, k)]);
}

void vfpu_matrix_write(int r, int n, float mat[4][4])
{
    int j, k;
    for (j = 0; j < n; j++)
        for (k = 0; k < n; k++)
            vfpu_vr[vfpu_matrix_lane(r, n, j, k)] = vfpu_as_bits(mat[j][k]);
}

void vfpu_unsupported_op(u32 op, u32 pc)
{
    vfpu_unsupported++;
    vfpu_last_unsupported = op;
    vfpu_last_unsupported_pc = pc;
}
