#ifndef TIMER_H
#define TIMER_H
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
    #endif
    #define DEFAULT_SPIN_US 600
    #define DEFAULT_IDLE_US 500
#else
    #include <sys/prctl.h>
    #define DEFAULT_SPIN_US 60
    #define DEFAULT_IDLE_US 50
#endif
#define MAX_SLEEP_US 2000

static inline double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;  // seconds
}
static inline void os_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// short waits, for a thread that has to wake up close to a moment in time (get_time() seconds). a wakeup can be
// late by the OS timer's jitter, so a caller sleeps up to a little before the moment and spins the rest
#if defined(_WIN32) || defined(_WIN64)
static HANDLE os_hr_timer = NULL;
static int os_hr_state = 0; // 0 not tried, 1 usable, -1 not available (before Windows 10 1803), os_sleep_until then returns at once
static inline void os_timer_init(void)
{
    if (os_hr_state) return;
    // a high resolution timer is about 0.5 ms accurate without touching the system wide timer resolution
    os_hr_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    os_hr_state = os_hr_timer ? 1 : -1;
}
static inline void os_sleep_until(double t)
{
    if (os_hr_state != 1) return;
    double dt = t - get_time();
    if (dt <= 0.0) return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(dt * 1e7); // negative: relative, in 100 ns
    if (SetWaitableTimer(os_hr_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(os_hr_timer, INFINITE);
}
#else
static inline void os_timer_init(void)
{
    prctl(PR_SET_TIMERSLACK, 1UL); // this thread's wakeups are otherwise allowed to be 50 us late
}
static inline void os_sleep_until(double t)
{
    struct timespec ts = { (time_t)t, (long)((t - (double)(time_t)t) * 1e9) };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}
#endif

static inline void os_cpu_relax(void)
{
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
    #endif
}
#endif
