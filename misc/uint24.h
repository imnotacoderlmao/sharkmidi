#ifndef uint24
#define uint24
#include <stdint.h>

typedef struct __attribute__((packed)) 
{
    uint16_t val1;
    uint8_t val2;
} uint24_t;

static inline int32_t uint24_get(const uint24_t *u) 
{
    return (int32_t)u->val1 | ((int32_t)u->val2 << 16);
}

static inline void uint24_set(uint24_t *u, int32_t value) 
{
    u->val1 = (uint16_t)(value & 0xFFFF);
    u->val2 = (uint8_t)((value >> 16) & 0x00FF);
}

static inline uint24_t uint24_from(int value) 
{
    uint24_t u;
    uint24_set(&u, value);
    return u;
}
#endif