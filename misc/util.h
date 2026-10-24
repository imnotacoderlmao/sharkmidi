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

#define ROLLING_SIZE 30
typedef struct 
{
    double values[ROLLING_SIZE];
    double sum;
    int head;
    int count;
} RollingAvg;

static double rolling_avg_update(RollingAvg* ra, double new_val) 
{
    if (ra->count < ROLLING_SIZE) 
    {
        ra->sum += new_val;
        ra->values[ra->head] = new_val;
        ra->count++;
    } else 
    {
        ra->sum -= ra->values[ra->head]; // Drop oldest value
        ra->sum += new_val;              // Add new value
        ra->values[ra->head] = new_val;
    }
    ra->head = (ra->head + 1) % ROLLING_SIZE;
    return ra->sum / ra->count;
}

#endif
