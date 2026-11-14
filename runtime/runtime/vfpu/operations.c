#include "vfpu_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

void psbridge_vfpu_dispatch(u32 op, u32 pc, u32 *gpr)
{
    int size = 1 + ((op >> 7) & 1) + (((op >> 15) & 1) << 1);
    int vd = op & 127, vs = (op >> 8) & 127, vt = (op >> 16) & 127;
    int major = op >> 26, subgroup = (op >> 23) & 7;
    float s[4] = {0}, t[4] = {0}, d[4] = {0};
    int i, j, k;
    vfpu_executed++;
    if (pc >= 0x0883abc8 && pc <= 0x0883e270)
    {
        u32 sp = gpr[29];
        if (sp && sp < 0x02000000 && sp + 20512 >= sp && sp + 20512 < 0x02000000)
        {
            u32 ptr = *(u32 *)(uintptr_t)(sp + 20508);
            if (!psbridge_mixer_sp)
            {
                psbridge_mixer_sp = sp;
                psbridge_mixer_pointer = ptr;
            }
        }
    }

    if (major == 53 || major == 61)
    {
        int base = (op >> 21) & 31;
        int reg = ((op >> 16) & 31) | ((op & 1) << 5);
        int offset = (int)(short)(op & 0xfffc);
        u32 addr = gpr[base] + offset;
        int part = (addr >> 2) & 3;
        int left = !(op & 2);
        int first = left ? 3 - part : 0;
        int count = left ? part + 1 : 4 - part;
        for (i = 0; i < count; i++)
        {
            int lane = first + i;
            u32 address = left ? addr - (u32)(4 * (count - 1 - i)) : addr + (u32)(4 * i);
            u32 *ptr = vfpu_checked_pointer(address, 4);
            if (!ptr)
                return;
            if (major == 53)
                vfpu_vr[vfpu_vector_lane(reg, 4, lane)] = *ptr;
            else
                *ptr = vfpu_vr[vfpu_vector_lane(reg, 4, lane)];
        }
        return;
    }
    if (major == 50 || major == 54 || major == 58 || major == 62)
    {
        int base = (op >> 21) & 31;
        int n = (major == 54 || major == 62) ? 4 : 1;
        int reg = ((op >> 16) & 31) | ((op & ((n == 4) ? 1 : 3)) << 5);
        int offset = (int)(short)(op & 0xfffc);
        u32 addr = gpr[base] + offset;
        u32 *p = (u32 *)vfpu_checked_pointer(addr, n * 4);
        if (!p)
            return;
        for (i = 0; i < n; i++)
        {
            int r = vfpu_vector_lane(reg, n, i);
            if (major == 50 || major == 54)
                vfpu_vr[r] = p[i];
            else
                p[i] = vfpu_vr[r];
        }
        return;
    }
    if ((op & 0xff000000) == 0xdc000000)
    {
        vfpu_prefix_s = op & 0xfffff;
        vfpu_have_s = 1;
        vfpu_ctrl[0] = vfpu_prefix_s;
        return;
    }
    if ((op & 0xff000000) == 0xdd000000)
    {
        vfpu_prefix_t = op & 0xfffff;
        vfpu_have_t = 1;
        vfpu_ctrl[1] = vfpu_prefix_t;
        return;
    }
    if ((op & 0xff000000) == 0xde000000)
    {
        vfpu_prefix_d = op & 0xfff;
        vfpu_have_d = 1;
        vfpu_ctrl[2] = vfpu_prefix_d;
        return;
    }
    if ((op & 0xff800000) == 0xdf000000)
    {
        d[0] = (float)(short)(op & 0xffff);
        vfpu_write_vector((op >> 16) & 127, 1, d, 0);
        return;
    }
    if ((op & 0xff800000) == 0xdf800000)
    {
        u32 h = op & 0xffff;
        u32 sign = (h & 0x8000) << 16;
        int exp = (h >> 10) & 31;
        u32 val;
        if (exp == 0)
            val = sign;
        else if (exp == 31)
            val = sign | 0x7f800000u | ((h & 1023) << 13);
        else
            val = sign | ((exp + 112u) << 23) | ((h & 1023) << 13);
        d[0] = vfpu_as_float(val);
        vfpu_write_vector((op >> 16) & 127, 1, d, 0);
        return;
    }
    if ((op & 0xffe00000) == 0x48600000 || (op & 0xffe00000) == 0x48e00000)
    {
        int rt = (op >> 16) & 31, control = (op & 0x80) != 0, idx = op & 127;
        if ((op & 0xffe00000) == 0x48600000)
        {
            if (rt && (!control || idx < 16))
                gpr[rt] = control ? vfpu_ctrl[idx] : vfpu_vr[vfpu_vector_lane(idx, 1, 0)];
        }
        else
        {
            if (control)
            {
                u32 value = gpr[rt];
                if (idx <= 1)
                {
                    value &= 0x000fffffu;
                    vfpu_ctrl[idx] = value;
                    if (idx == 0)
                    {
                        vfpu_prefix_s = value;
                        vfpu_have_s = 1;
                    }
                    else
                    {
                        vfpu_prefix_t = value;
                        vfpu_have_t = 1;
                    }
                }
                else if (idx == 2)
                {
                    vfpu_prefix_d = value & 0xfffu;
                    vfpu_ctrl[2] = vfpu_prefix_d;
                    vfpu_have_d = 1;
                }
                else if (idx == 3)
                    vfpu_ctrl[3] = value & 0x3fu;
                else if (idx == 4)
                    vfpu_ctrl[4] = value;
                else if (idx >= 8 && idx <= 15)
                    vfpu_ctrl[idx] = (value & 0xfffffu) | 0x3f800000u;
            }
            else
                vfpu_vr[vfpu_vector_lane(idx, 1, 0)] = gpr[rt];
        }
        return;
    }
    if (op == 0xffff0000 || op == 0xffff0320 || op == 0xffff040d)
        return;
    if (major >= 24 && major <= 27)
    {
        vfpu_read_prefix(vs, size, s, vfpu_prefix_s, vfpu_have_s);
        vfpu_read_prefix(vt, size, t, vfpu_prefix_t, vfpu_have_t);
        switch (major)
        {
        case 24:
            if (subgroup == 0)
                for (i = 0; i < size; i++)
                    d[i] = s[i] + t[i];
            else if (subgroup == 1)
                for (i = 0; i < size; i++)
                    d[i] = s[i] - t[i];
            else if (subgroup == 2)
                for (i = 0; i < size; i++)
                    d[i] = s[i] * exp2f(t[i]);
            else if (subgroup == 7)
                for (i = 0; i < size; i++)
                    d[i] = t[i] == 0 ? 0 : s[i] / t[i];
            else
                goto unknown;
            break;
        case 25:
            if (subgroup == 0)
                for (i = 0; i < size; i++)
                    d[i] = s[i] * t[i];
            else if (subgroup == 1)
            {
                float dot = 0;
                for (i = 0; i < size; i++)
                    dot += s[i] * t[i];
                d[0] = dot;
                size = 1;
            }
            else if (subgroup == 2)
            {
                float scalar[4] = {0};
                static const float constants[8] = {
                    0.0f, 1.0f, 2.0f, 0.5f,
                    3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f
                };
                vfpu_read_vector(vt, 1, scalar);
                for (i = 0; i < size; i++)
                {
                    float scale = scalar[0];
                    if (vfpu_have_t)
                    {
                        int swizzle = (vfpu_prefix_t >> (i * 2)) & 3;
                        int absolute = (vfpu_prefix_t >> (8 + i)) & 1;
                        if (vfpu_prefix_t & (1u << (12 + i)))
                            scale = constants[swizzle + (absolute << 2)];
                        else if (absolute)
                            scale = fabsf(scale);
                        if (vfpu_prefix_t & (1u << (16 + i)))
                            scale = -scale;
                    }
                    d[i] = s[i] * scale;
                }
            }
            else if (subgroup == 4)
            {
                float dot = 0;
                for (i = 0; i < size - 1; i++)
                    dot += s[i] * t[i];
                d[0] = dot + t[size - 1];
                size = 1;
            }
            else if (subgroup == 5 && size == 3)
            {
                d[0] = s[1] * t[2];
                d[1] = s[2] * t[0];
                d[2] = s[0] * t[1];
            }
            else if (subgroup == 6 && size == 2)
            {
                d[0] = s[0] * t[1] - s[1] * t[0];
                size = 1;
            }
            else
                goto unknown;
            break;
        case 26:
            goto unknown;
        case 27:
            if (subgroup == 0)
            {
                u32 cc = 0;
                int cond = op & 15;
                for (i = 0; i < size; i++)
                {
                    int hit = 0;
                    switch (cond)
                    {
                    case 0:
                        break;
                    case 1:
                        hit = s[i] == t[i];
                        break;
                    case 2:
                        hit = s[i] < t[i];
                        break;
                    case 3:
                        hit = s[i] <= t[i];
                        break;
                    case 4:
                        hit = 1;
                        break;
                    case 5:
                        hit = s[i] != t[i];
                        break;
                    case 6:
                        hit = s[i] >= t[i];
                        break;
                    case 7:
                        hit = s[i] > t[i];
                        break;
                    case 8:
                        hit = s[i] == 0;
                        break;
                    case 9:
                        hit = isnan(s[i]);
                        break;
                    case 10:
                        hit = isinf(s[i]);
                        break;
                    case 11:
                        hit = isnan(s[i]) || isinf(s[i]);
                        break;
                    case 12:
                        hit = s[i] != 0.0f;
                        break;
                    case 13:
                        hit = !isnan(s[i]);
                        break;
                    case 14:
                        hit = !isinf(s[i]);
                        break;
                    case 15:
                        hit = !isnan(s[i]) && !isinf(s[i]);
                        break;
                    }
                    cc |= (hit ? 1u : 0u) << i;
                }
                {
                    u32 mask = ((1u << size) - 1u) | 0x30u;
                    u32 previous = vfpu_ctrl[3];
                    u32 current = cc | ((cc & ((1u << size) - 1u)) ? 0x10u : 0u) |
                        ((cc & ((1u << size) - 1u)) == ((1u << size) - 1u) ? 0x20u : 0u);
                    vfpu_ctrl[3] = (previous & ~mask) | (current & mask);
                }
                vfpu_finish_prefix();
                return;
            }
            if (subgroup == 2)
                for (i = 0; i < size; i++)
                    d[i] = fminf(s[i], t[i]);
            else if (subgroup == 3)
                for (i = 0; i < size; i++)
                    d[i] = fmaxf(s[i], t[i]);
            else if (subgroup == 5)
                for (i = 0; i < size; i++)
                    d[i] = s[i] > t[i] ? 1.0f : s[i] < t[i] ? -1.0f : 0.0f;
            else if (subgroup == 6)
                for (i = 0; i < size; i++)
                    d[i] = s[i] >= t[i] ? 1 : 0;
            else if (subgroup == 7)
                for (i = 0; i < size; i++)
                    d[i] = s[i] < t[i] ? 1 : 0;
            else
                goto unknown;
            break;
        }
        vfpu_write_vector(vd, size, d, 1);
        vfpu_finish_prefix();
        return;
    }
    if ((op & 0xffe00000) == 0xd0600000)
    {
        static const float values[32] = {
            0.0f, 3.402823466e38f, 1.4142135623730951f, 0.7071067811865476f,
            1.1283791670955126f, 0.6366197723675813f, 0.3183098861837907f,
            0.7853981633974483f, 1.5707963267948966f, 3.1415926535897932f,
            2.7182818284590452f, 1.4426950408889634f, 0.4342944819032518f,
            0.6931471805599453f, 2.3025850929940457f, 6.2831853071795865f,
            0.5235987755982989f, 0.3010299956639812f, 3.3219280948873623f,
            0.8660254037844386f
        };
        float c = values[(op >> 16) & 31];
        for (i = 0; i < size; i++)
            d[i] = c;
        vfpu_write_vector(vd, size, d, 1);
        vfpu_finish_prefix();
        return;
    }
    if ((op & 0xff000000) == 0xd0000000)
    {
        int kind = (op >> 16) & 255;
        vfpu_read_prefix(vs, size, s, vfpu_prefix_s, vfpu_have_s);
        if (kind == 66 || kind == 67)
        {
            vfpu_read_vector(vs, size, s);
            if (kind == 66 && (size == 2 || size == 4))
            {
                for (i = 0; i < size; i += 2)
                {
                    d[i] = s[i] + s[i + 1];
                    d[i + 1] = s[i] - s[i + 1];
                }
            }
            else if (kind == 67 && size == 4)
            {
                d[0] = s[0] + s[2];
                d[1] = s[1] + s[3];
                d[2] = s[0] - s[2];
                d[3] = s[1] - s[3];
            }
            else
                goto unknown;
            vfpu_write_vector(vd, size, d, 1);
            vfpu_finish_prefix();
            return;
        }
        if (kind == 70 || kind == 71)
        {
            float total = 0.0f;
            for (i = 0; i < size; i++)
                total += s[i];
            d[0] = kind == 71 ? total / (float)size : total;
            vfpu_write_vector(vd, 1, d, 1);
            vfpu_finish_prefix();
            return;
        }
        for (i = 0; i < size; i++)
        {
            float v = s[i];
            switch (kind)
            {
            case 0:
                d[i] = v;
                break;
            case 1:
                d[i] = fabsf(v);
                break;
            case 2:
                d[i] = -v;
                break;
            case 4:
                d[i] = fminf(1, fmaxf(0, v));
                break;
            case 5:
                d[i] = fminf(1, fmaxf(-1, v));
                break;
            case 3:
                d[i] = (vd & (size - 1)) == i ? 1 : 0;
                break;
            case 6:
                d[i] = 0;
                break;
            case 7:
                d[i] = 1;
                break;
            case 16:
                d[i] = v ? 1 / v : 0;
                break;
            case 17:
                d[i] = v > 0 ? 1 / sqrtf(v) : 0;
                break;
            case 18:
                d[i] = sinf(v * 1.57079632679f);
                break;
            case 19:
                d[i] = cosf(v * 1.57079632679f);
                break;
            case 20:
                d[i] = exp2f(v);
                break;
            case 21:
                d[i] = v > 0 ? log2f(v) : 0;
                break;
            case 22:
                d[i] = sqrtf(fmaxf(v, 0));
                break;
            case 23:
                d[i] = asinf(fmaxf(-1, fminf(1, v))) / 1.57079632679f;
                break;
            case 24:
                d[i] = v ? -1 / v : 0;
                break;
            case 26:
                d[i] = -sinf(v * 1.57079632679f);
                break;
            case 28:
                d[i] = exp2f(-v);
                break;
            case 68:
                d[i] = 1 - v;
                break;
            case 70:
                d[i] = 0;
                break;
            case 71:
                d[i] = v;
                break;
            case 74:
                d[i] = v < 0 ? -1 : v > 0 ? 1 : 0;
                break;
            default:
                goto unknown;
            }
        }
        vfpu_write_vector(vd, size, d, 1);
        vfpu_finish_prefix();
        return;
    }
    if ((op & 0xff000000) == 0xd2000000 || (op & 0xffe00000) == 0xd2000000 ||
        (op & 0xffe00000) == 0xd2200000 || (op & 0xffe00000) == 0xd2400000 ||
        (op & 0xffe00000) == 0xd2600000 || (op & 0xffe00000) == 0xd2800000)
    {
        int kind = (op >> 21) & 7, scale = (op >> 16) & 31;
        vfpu_read_prefix(vs, size, s, vfpu_prefix_s, vfpu_have_s);
        if (kind <= 3)
        {
            for (i = 0; i < size; i++)
            {
                float v = ldexpf(s[i], scale);
                if (isnan(v))
                    vfpu_vr[vfpu_vector_lane(vd, size, i)] = 0x7fffffff;
                else if (v >= 2147483647.0f)
                    vfpu_vr[vfpu_vector_lane(vd, size, i)] = 0x7fffffff;
                else if (v <= -2147483648.0f)
                    vfpu_vr[vfpu_vector_lane(vd, size, i)] = 0x80000000;
                else
                {
                    float r = kind == 0   ? nearbyintf(v)
                              : kind == 1 ? truncf(v)
                              : kind == 2 ? ceilf(v)
                                          : floorf(v);
                    vfpu_vr[vfpu_vector_lane(vd, size, i)] = (u32)(s32)r;
                }
            }
        }
        else if (kind == 4)
        {
            for (i = 0; i < size; i++)
                d[i] = ldexpf((float)(s32)vfpu_vr[vfpu_vector_lane(vs, size, i)], -scale);
            vfpu_write_vector(vd, size, d, 1);
        }
        else if (kind == 5)
        {
            int bit = (op >> 16) & 7;
            int move_when_false = (op & 0x00080000) != 0;
            float previous[4] = {0};
            vfpu_read_prefix(vd, size, previous, vfpu_prefix_t, vfpu_have_t);
            for (i = 0; i < size; i++)
            {
                int cond = bit == 6 ? ((vfpu_ctrl[3] >> i) & 1) :
                    bit == 7 ? 0 : ((vfpu_ctrl[3] >> bit) & 1);
                d[i] = cond == !move_when_false ? s[i] : previous[i];
            }
            vfpu_write_vector(vd, size, d, 1);
        }
        else
            goto unknown;
        vfpu_finish_prefix();
        return;
    }
    if ((op & 0xff800000) == 0xf2800000 && (size == 3 || size == 4))
    {
        vfpu_read_vector(vs, size, s);
        vfpu_read_vector(vt, size, t);
        if (size == 3)
        {
            d[0] = s[1] * t[2] - s[2] * t[1];
            d[1] = s[2] * t[0] - s[0] * t[2];
            d[2] = s[0] * t[1] - s[1] * t[0];
        }
        else
        {
            d[0] = s[3] * t[0] - s[2] * t[1] + s[1] * t[2] + s[0] * t[3];
            d[1] = s[3] * t[1] + s[2] * t[0] + s[1] * t[3] - s[0] * t[2];
            d[2] = s[3] * t[2] + s[2] * t[3] - s[1] * t[0] + s[0] * t[1];
            d[3] = s[3] * t[3] - s[2] * t[2] - s[1] * t[1] - s[0] * t[0];
        }
        vfpu_write_vector(vd, size, d, 1);
        vfpu_finish_prefix();
        return;
    }
    if ((op & 0xff000000) == 0xf0000000 || (op & 0xff000000) == 0xf1000000 ||
        (op & 0xff000000) == 0xf2000000 || (op & 0xff000000) == 0xf3000000)
    {
        float a[4][4] = {{0}}, b[4][4] = {{0}}, o[4][4] = {{0}};
        int subtype = (op >> 23) & 7;
        if ((op & 0xffe00000) == 0xf3a00000)
        {
            int imm = (op >> 16) & 31;
            int sin_lane = (imm >> 2) & 3;
            int cos_lane = imm & 3;
            float angle[4] = {0};
            float original[4] = {0};
            float si, co;
            vfpu_read_vector(vs, 1, original);
            vfpu_read_prefix(vs, 1, angle, vfpu_prefix_s, vfpu_have_s);
            si = sinf(angle[0] * 1.5707963267948966f);
            co = cosf(original[0] * 1.5707963267948966f);
            if (imm & 16)
                si = -si;
            for (i = 0; i < size; i++)
                d[i] = (sin_lane == cos_lane || i == sin_lane) ? si : 0.0f;
            for (i = 0; i < size; i++)
                if (vfpu_vector_lane(vd, size, i) == vfpu_vector_lane(vs, 1, 0))
                {
                    co = cosf(d[i] * 1.5707963267948966f);
                    break;
                }
            if (cos_lane < size)
                d[cos_lane] = co;
            if (vfpu_have_d && cos_lane < size)
            {
                vfpu_prefix_d &= ~((3u << (cos_lane * 2)) | (1u << (8 + cos_lane)));
            }
            vfpu_write_vector(vd, size, d, 1);
            vfpu_finish_prefix();
            return;
        }
        if ((op & 0xff800000) == 0xf0800000 || (op & 0xff800000) == 0xf1000000 ||
            (op & 0xff800000) == 0xf1800000)
        {
            int n = size;
            int homogeneous = n == subtype;
            float input[4] = {0};
            if (homogeneous)
                n++;
            else if (n != subtype + 1)
                goto unknown;
            vfpu_matrix_read(vs, n, a);
            vfpu_read_vector(vt, n, input);
            for (i = 0; i < n; i++)
            {
                float accum = 0;
                for (k = 0; k < n; k++)
                    accum += a[i][k] * (homogeneous && k == n - 1 ? 1 : input[k]);
                d[i] = accum;
            }
            vfpu_write_vector(vd, n, d, 1);
            vfpu_finish_prefix();
            return;
        }
        if ((op & 0xff800000) == 0xf0000000)
        {
            vfpu_matrix_read(vs, size, a);
            vfpu_matrix_read(vt, size, b);
            for (j = 0; j < size; j++)
                for (i = 0; i < size; i++)
                    for (k = 0; k < size; k++)
                        o[j][i] += a[i][k] * b[j][k];
            vfpu_matrix_write(vd, size, o);
            vfpu_finish_prefix();
            return;
        }
        if ((op & 0xff800000) == 0xf2000000)
        {
            vfpu_matrix_read(vs, size, a);
            vfpu_read_vector(vt, 1, t);
            for (j = 0; j < size; j++)
                for (i = 0; i < size; i++)
                    o[j][i] = a[j][i] * t[0];
            vfpu_matrix_write(vd, size, o);
            vfpu_finish_prefix();
            return;
        }
        if ((op & 0xffff0000) == 0xf3800000)
        {
            vfpu_matrix_read(vs, size, a);
            vfpu_matrix_write(vd, size, a);
            vfpu_finish_prefix();
            return;
        }
        if ((op & 0xffff0000) == 0xf3830000 || (op & 0xffff0000) == 0xf3860000 ||
            (op & 0xffff0000) == 0xf3870000)
        {
            for (j = 0; j < size; j++)
                for (i = 0; i < size; i++)
                    o[j][i] = ((op >> 16) & 255) == 0x83   ? (i == j)
                              : ((op >> 16) & 255) == 0x87 ? 1
                                                           : 0;
            vfpu_matrix_write(vd, size, o);
            vfpu_finish_prefix();
            return;
        }
    }
unknown:
    vfpu_unsupported_op(op, pc);
}

