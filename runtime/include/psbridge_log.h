#ifndef PSBRIDGE_LOG_H
#define PSBRIDGE_LOG_H

#include <stdio.h>
#include <stdarg.h>

static inline void psbridge_print_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

#define PRINT_LOG(...) do { } while (0)
#define PRINT_3D_LOG(...) psbridge_print_log(__VA_ARGS__)

#define PSB_LOG_MASTER 0
#define PSB_LOG_IMPORT 0
#define PSB_LOG_IO 0
#define PSB_LOG_THREAD 1
#define PSB_LOG_MBX 1
#define PSB_LOG_GE 1
#define PSB_LOG_GFX 0
#define PSB_LOG_AUDIO 0
#define PSB_LOG_MEMORY 0
#define PSB_LOG_VFPU 0

#if PSB_LOG_MASTER
#define PSB_LOG(area, format, ...) do { if (PSB_LOG_##area) PRINT_LOG(format "\n", ##__VA_ARGS__); } while (0)
#else
#define PSB_LOG(area, format, ...) do { } while (0)
#endif

#endif
