// typed-array.h
#define DEFINE_ARRAY(T)                                                      \
typedef struct                                                               \
{                                                                            \
    T* data;                                                                 \
    int64_t count;                                                           \
    int64_t cap;                                                             \
} T##_arr;                                                                   \
static inline void T##_arr_push(T##_arr* a, T item)                          \
{                                                                            \
    if (a->count >= a->cap)                                                  \
    {                                                                        \
        a->cap = a->cap < 1024 ? 1024 : a->cap * 2;                          \
        a->data = realloc(a->data, a->cap * sizeof(T));                      \
    }                                                                        \
    a->data[a->count++] = item;                                              \
}                                                                            \
static inline void T##_arr_free(T##_arr* a)                                  \
{                                                                            \
    free(a->data);                                                           \
    *a = (T##_arr){0};                                                       \
}                                                                            