#ifndef UTIL_H
#define UTIL_H

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define ROLLING_SIZE 60
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
        ra->sum -= ra->values[ra->head];
        ra->sum += new_val;
        ra->values[ra->head] = new_val;
    }
    ra->head = (ra->head + 1) % ROLLING_SIZE;
    return ra->sum / ra->count;
}


#endif
