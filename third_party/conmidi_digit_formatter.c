#include <stdio.h>
#include <string.h>
#include <inttypes.h>

const char* __attribute__((noinline)) AddCommas(int64_t num)
{
    #define COMMAS_RING_SIZE 16
    #define COMMAS_BUF_SIZE  32

    // thread local ring buffer lets you call fmt_commas() multiple times in one printf()
    static _Thread_local char ring[COMMAS_RING_SIZE][COMMAS_BUF_SIZE];
    static _Thread_local int ring_idx = 0;

    char* output = ring[ring_idx];
    ring_idx = (ring_idx + 1) % COMMAS_RING_SIZE;

    char temp[32];
    snprintf(temp, sizeof(temp), "%" PRId64, num);
    
    int len = (int)strlen(temp);
    int commas = (len - 1) / 3;
    int new_len = len + commas;
    output[new_len] = '\0';

    int i = len - 1;
    int j = new_len - 1;
    int count = 0;

    while (i >= 0) 
    {
        if (count == 3 && temp[i] >= '0' && temp[i] <= '9') 
        {
            output[j--] = ',';
            count = 0;
        }
        output[j--] = temp[i--];
        count++;
    }

    return output;
}