#include <time.h>
static inline double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;  // seconds
}
#if defined(_WIN32) || defined(_WIN64)
static void os_sleep_ms(int ms) { Sleep(ms); }
#else
static void os_sleep_ms(int ms) 
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

