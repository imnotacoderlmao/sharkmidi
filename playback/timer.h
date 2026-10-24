#ifndef TIMER_H
#define TIMER_H
#include <time.h>
#include <stdint.h>

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

// -DUSE_TSC_CLOCK (x86 gcc/clang): get_time() reads the CPU's time stamp counter, about half the cost of
// clock_gettime. the counter is calibrated once against CLOCK_MONOTONIC and used only when the CPU reports an
// invariant TSC, otherwise get_time() is the plain clock_gettime. call timer_calibrate() once at startup (main), if
// not it runs on the first get_time() and the calling thread waits TSC_CAL_MS for it
#if defined(USE_TSC_CLOCK) && (defined(__x86_64__) || defined(__i386__))
#define TSC_CLOCK 1
#include <x86intrin.h>
#include <cpuid.h>
#include <stdatomic.h>
#ifndef TSC_CAL_MS
#define TSC_CAL_MS 100 // the calibration error is about 100 ns / this, 1 ppm at 100 ms
#endif
#endif

static inline double mono_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;  // seconds
}

#ifdef TSC_CLOCK
// the calibration is shared by every file that includes this header: exactly one .c file defines TIMER_IMPL before
// including it (playback_thread.c, say)
extern atomic_int tsc_state;        // 0 not calibrated, 1 being calibrated, 2 usable, -1 not usable
extern uint64_t tsc_base;
extern double tsc_time_base, tsc_period;
void timer_calibrate(void);
#ifdef TIMER_IMPL
atomic_int tsc_state;
uint64_t tsc_base;
double tsc_time_base, tsc_period;
void timer_calibrate(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&tsc_state, &expected, 1))
    {
        while (atomic_load(&tsc_state) == 1) { struct timespec ms = { 0, 1000000L }; nanosleep(&ms, NULL); }
        return;
    }
    unsigned a, b, c, d;
    int ok = __get_cpuid(0x80000000u, &a, &b, &c, &d) && a >= 0x80000007u && __get_cpuid(0x80000007u, &a, &b, &c, &d) && (d & (1u << 8)); // invariant TSC
    if (ok)
    {
        // each end is read as tsc, clock, tsc: the clock is taken to be at the middle of its pair, and the pair with the
        // smallest gap is kept, so a preemption in between does not skew it
        double t0 = 0, t1 = 0; uint64_t c0 = 0, c1 = 0;
        for (int side = 0; side < 2; side++)
        {
            if (side) { struct timespec ms = { TSC_CAL_MS / 1000, (TSC_CAL_MS % 1000) * 1000000L }; nanosleep(&ms, NULL); }
            uint64_t best = ~0ull;
            for (int i = 0; i < 64; i++)
            {
                uint64_t x = __rdtsc(); double t = mono_time(); uint64_t y = __rdtsc();
                if (y - x < best) { best = y - x; if (side) { t1 = t; c1 = x + (y - x) / 2; } else { t0 = t; c0 = x + (y - x) / 2; } }
            }
        }
        ok = t1 > t0 && c1 > c0;
        if (ok) { tsc_base = c0; tsc_time_base = t0; tsc_period = (t1 - t0) / (double)(c1 - c0); }
    }
    atomic_store(&tsc_state, ok ? 2 : -1);
}
#endif
static inline double get_time(void)
{
    if (__builtin_expect(atomic_load_explicit(&tsc_state, memory_order_relaxed) != 2, 0))
    {
        if (atomic_load(&tsc_state) == 0 || atomic_load(&tsc_state) == 1) timer_calibrate();
        if (atomic_load(&tsc_state) != 2) return mono_time();
    }
    return tsc_time_base + (double)(__rdtsc() - tsc_base) * tsc_period; // seconds, same origin as CLOCK_MONOTONIC when calibrated
}
#else
static inline void timer_calibrate(void) {}
static inline double get_time(void) { return mono_time(); }
#endif
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
#ifdef TSC_CLOCK
    // t is in get_time() seconds, which drifts from CLOCK_MONOTONIC (that one is slewed by ntp): the absolute
    // time to sleep to is worked out from the time left
    t = mono_time() + (t - get_time());
#endif
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
