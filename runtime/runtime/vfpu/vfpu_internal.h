#ifndef PSBRIDGE_VFPU_INTERNAL_H
#define PSBRIDGE_VFPU_INTERNAL_H

#include "psbridge.h"
#include <tamtypes.h>

extern u32 vfpu_vr[128];
extern u32 vfpu_ctrl[16];
extern u32 vfpu_prefix_s;
extern u32 vfpu_prefix_t;
extern u32 vfpu_prefix_d;
extern u8 vfpu_have_s;
extern u8 vfpu_have_t;
extern u8 vfpu_have_d;
extern u32 vfpu_executed;
extern u32 vfpu_unsupported;
extern u32 vfpu_invalid_memory;
extern u32 vfpu_last_unsupported;
extern u32 vfpu_last_unsupported_pc;
extern u32 vfpu_last_invalid_address;


float vfpu_as_float(u32 bits);
u32 vfpu_as_bits(float value);
int vfpu_vector_lane(int reg, int size, int lane);
int vfpu_matrix_lane(int reg, int size, int column, int row);
void vfpu_read_vector(int reg, int size, float output[4]);
void vfpu_read_prefix(int reg, int size, float output[4], u32 prefix, int active);
void vfpu_write_vector(int reg, int size, const float input[4], int use_prefix);
void vfpu_finish_prefix(void);
void vfpu_matrix_read(int reg, int size, float matrix[4][4]);
void vfpu_matrix_write(int reg, int size, float matrix[4][4]);
void vfpu_unsupported_op(u32 op, u32 pc);
void *vfpu_checked_pointer(u32 raw, u32 bytes);

#endif
