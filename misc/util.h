#ifndef UTIL_H
#define UTIL_H
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
