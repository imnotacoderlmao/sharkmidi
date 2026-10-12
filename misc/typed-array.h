#ifndef TYPED_ARRAY_H
#define TYPED_ARRAY_H
#include <stdint.h>
#include <stdlib.h>
#define DEFINE_ARRAY(T)                                                      \
typedef struct                                                               \
{                                                                            \
    T* data;                                                                 \
    int64_t count;                                                           \
    int64_t cap;                                                             \
} T##_arr;                                                                   \
__attribute__((noinline, unused))                                            \
static void T##_arr_push(T##_arr* a, T item)                                 \
{                                                                            \
    if (a->count >= a->cap)                                                  \
    {                                                                        \
        a->cap = a->cap < 1024 ? 1024 : a->cap * 2;                          \
        a->data = realloc(a->data, a->cap * sizeof(T));                      \
    }                                                                        \
    a->data[a->count++] = item;                                              \
}                                                                            \
__attribute__((noinline))                                                    \
static void T##_arr_free(T##_arr* a)                                         \
{                                                                            \
    free(a->data);                                                           \
    *a = (T##_arr){0};                                                       \
}                                                                            \
__attribute__((noinline, unused))                                            \
static void T##_arr_shrink(T##_arr* a)                                       \
{                                                                            \
    if (a->count == 0)                                                       \
    {                                                                        \
        free(a->data);                                                       \
        *a = (T##_arr){0};                                                   \
        return;                                                              \
    }                                                                        \
    if (a->cap > a->count)                                                   \
    {                                                                        \
        void* p = realloc(a->data, a->count * sizeof(T));                    \
        if (p) a->data = p;                                                  \
        a->cap = a->count;                                                   \
    }                                                                        \
}                                                                            
#endif
