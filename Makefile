CC = gcc
CFLAGS = -O3 -fstrict-overflow -fno-semantic-interposition -fno-plt -flto -pthread -fno-math-errno

native = $(shell echo $(do_compile_native) | tr A-Z a-z)
do_openmp = $(shell echo $(multiprocessing) | tr A-Z a-z)
clang = $(shell echo $(clang_compile) | tr A-Z a-z)

ifeq (${native},true)
    CFLAGS += -march=native
    CFLAGS_PLAYBACK += -march=native
endif

ifeq (${do_openmp},true)
    CFLAGS += -fopenmp
endif

ifeq (${clang},true)
    CC = clang
endif

SRCS = $(wildcard *.c) $(wildcard parse/*.c) $(wildcard playback/*.c) $(wildcard synth/*.c)

OBJS = $(patsubst %.c, build/%.o, $(SRCS))
TARGET = build/test.out

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)