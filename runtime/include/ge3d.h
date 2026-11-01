#ifndef PSBRIDGE_GE3D_H
#define PSBRIDGE_GE3D_H

#include <tamtypes.h>

typedef struct {
    float x;
    float y;
    float u;
    float v;
    float z;
    u32 color;
} GeVertex;

void ge3d_matrix_command(u32 command, u32 value);
void ge3d_command(u32 command, u32 value);
void ge3d_transform_vertex(GeVertex *vertex, u32 type, const u32 *commands);

#endif
