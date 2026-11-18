CC ?= gcc
CFLAGS = -O3 -fstrict-overflow -fno-math-errno -flto=auto -fno-stack-protector -fno-semantic-interposition -fno-plt -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident -Wl,-rpath,'$$ORIGIN' -s
# libmath now linked due to exp()
LDFLAGS = -pthread -lm -Wl,--gc-sections -Wl,--build-id=none -flto=auto

# for cross compilation purposes
IS_MINGW = $(shell $(CC) -dumpmachine 2>&1 | grep -E "mingw|w64")
native = $(shell echo $(do_compile_native) | tr A-Z a-z)
do_openmp = $(shell echo $(multiprocessing) | tr A-Z a-z)
dbg = $(shell echo $(debug) | tr A-Z a-z)
tsc = $(shell echo $(use_tsc) | tr A-Z a-z)
is_headless = $(shell echo $(headless) | tr A-Z a-z)

SRCS = $(wildcard *.c) $(wildcard parse/*.c) $(wildcard playback/*.c) $(wildcard synth/*.c) $(wildcard third_party/*.c)
OBJS = $(patsubst %.c, build/%.o, $(SRCS))

ifeq (${tsc}, true)
    CFLAGS += -DUSE_TSC_CLOCK
endif

ifeq (${dbg},true)
    CFLAGS = -g -O0 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined
    LDFLAGS = -g -pthread -lm
endif

ifeq (${native},true)
    CFLAGS += -march=native
endif

ifeq (${do_openmp},true)
    CFLAGS += -fopenmp
    LDFLAGS += -fopenmp
endif

ifeq (${is_headless},false)
    CFLAGS += -DNOT_HEADLESS
    SRCS += $(wildcard render/*.c) $(wildcard third_party/glad/src/*.c)
    ifeq ($(strip $(IS_MINGW)),)
        LDFLAGS += -lglfw -lGL
    else
        GLFW_WIN_DIR ?=
        ifeq (${GLFW_WIN_DIR},)
            $(error GLFW_WIN_DIR not set. stopping compilation)
        endif
        LDFLAGS += -L$(GLFW_WIN_DIR)/lib-mingw-w64 -lglfw3 -mconsole -lopengl32 -lkernel32 -lgdi32 -luser32 -lkernel32
        CFLAGS += -I$(GLFW_WIN_DIR)/include
    endif
endif

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
	rm -rf build/*.o build/parse build/playback build/synth build/render build/third_party