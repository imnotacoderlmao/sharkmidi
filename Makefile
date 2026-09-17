CC ?= gcc
CFLAGS = -O3 -fstrict-overflow -fno-math-errno -pthread -flto
LDFLAGS = -pthread -flto

# for cross compilation purposes
IS_MINGW = $(shell $(CC) -dumpmachine 2>&1 | grep -E "mingw|w64")

ifeq ($(strip $(IS_MINGW)),)
    CFLAGS += -fno-semantic-interposition -fno-plt
    LDFLAGS += -lglfw -lGL
else
    GLFW_WIN_DIR ?=
    ifeq (${GLFW_WIN_DIR},)
        $(error GLFW_WIN_DIR not set. stopping compilation)
    endif
    LDFLAGS += -mwindows -lopengl32 -lkernel32
    CFLAGS += -I$(GLFW_WIN_DIR)/include
    LDFLAGS += -L$(GLFW_WIN_DIR)/lib-mingw-w64 -lglfw3
endif

native = $(shell echo $(do_compile_native) | tr A-Z a-z)
do_openmp = $(shell echo $(multiprocessing) | tr A-Z a-z)
dbg = $(shell echo $(debug_symbols) | tr A-Z a-z)

ifeq (${native},true)
    CFLAGS += -march=native
endif

ifeq (${do_openmp},true)
    CFLAGS += -fopenmp
    LDFLAGS += -fopenmp
endif

ifeq (${dbg},true)
    CFLAGS += -g
endif

SRCS = $(wildcard *.c) $(wildcard parse/*.c) $(wildcard playback/*.c) $(wildcard synth/*.c) $(wildcard render/*.c) $(wildcard third_party/glad/src/*.c) $(wildcard third_party/*.c)
OBJS = $(patsubst %.c, build/%.o, $(SRCS))

ifeq ($(strip $(IS_MINGW)),)
    TARGET = build/sharkmidi
else
    TARGET = build/sharkmidi.exe
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/
