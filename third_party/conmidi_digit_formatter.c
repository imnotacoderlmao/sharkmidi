#include <stdio.h>
#include <inttypes.h>

const char* __attribute__((noinline)) AddCommas(int64_t num)
{
    #define COMMAS_RING_SIZE 16
    #define COMMAS_BUF_SIZE  24

    // thread local ring buffer lets you call fmt_commas() multiple times in one printf()
    static _Thread_local char ring[COMMAS_RING_SIZE][COMMAS_BUF_SIZE];
    static _Thread_local int ring_idx = 0;

    char* output = ring[ring_idx];
    ring_idx = (ring_idx + 1) % COMMAS_RING_SIZE;

    // work on the unsigned magnitude: handles negatives (the old version left a garbage byte for e.g. -123) and INT64_MIN
    int neg = num < 0;
    uint64_t mag = neg ? 0 - (uint64_t)num : (uint64_t)num;
    char temp[24];
    int len = snprintf(temp, sizeof(temp), "%" PRIu64, mag);
    int new_len = neg + len + (len - 1) / 3;
    output[new_len] = '\0';

    int j = new_len - 1;
    for (int i = len - 1, count = 0; i >= 0; i--, count++)
    {
        if (count == 3)
        {
            output[j--] = ',';
            count = 0;
        }
        output[j--] = temp[i];
    }
    if (neg)
        output[0] = '-';
    return output;
}
