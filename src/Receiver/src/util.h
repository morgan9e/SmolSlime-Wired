#ifndef SLIMENRF_UTILS
#define SLIMENRF_UTILS

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884f
#endif

#ifndef EPS
#define EPS 1e-6
#endif

// Saturate int to 16 bits
// Optimized to a single ARM assembler instruction
#define SATURATE_INT16(x) ((x) > 32767 ? 32767 : ((x) < -32768 ? -32768 : (x)))

#define SATURATE_UINT11(x) ((x) > 2047 ? 2047 : ((x) < 0 ? 0 : (x)))
#define SATURATE_UINT10(x) ((x) > 1023 ? 1023 : ((x) < 0 ? 0 : (x)))

#define TO_FIXED_15(x) ((int16_t)SATURATE_INT16((x) * (1 << 15)))
#define TO_FIXED_11(x) ((int16_t)((x) * (1 << 11)))
#define TO_FIXED_10(x) ((int16_t)((x) * (1 << 10)))
#define TO_FIXED_7(x) ((int16_t)SATURATE_INT16((x) * (1 << 7)))
#define FIXED_15_TO_DOUBLE(x) (((double)(x)) / (1 << 15))
#define FIXED_11_TO_DOUBLE(x) (((double)(x)) / (1 << 11))
#define FIXED_10_TO_DOUBLE(x) (((double)(x)) / (1 << 10))
#define FIXED_7_TO_DOUBLE(x) (((double)(x)) / (1 << 7))

#define CONST_EARTH_GRAVITY 9.80665

#endif