#ifndef UTIL_H
#define UTIL_H
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#if defined(_WIN32) || defined(_WIN64)
    #define DEFAULT_SPIN_US 600
    #define DEFAULT_IDLE_US 500
#else
    #define DEFAULT_SPIN_US 60
    #define DEFAULT_IDLE_US 50
#endif
#define MAX_SLEEP_US 2000
#endif
