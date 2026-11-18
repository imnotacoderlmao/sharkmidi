CC ?= gcc
BUILD_DIR := build

to_lower = $(shell printf '%s' "$(1)" | tr '[:upper:]' '[:lower:]')
do_compile_native ?= false
multiprocessing ?= false
debug ?= false
use_tsc ?= false
headless ?= false

IS_NATIVE   := $(call to_lower,$(do_compile_native))
USE_OPENMP  := $(call to_lower,$(multiprocessing))
IS_DEBUG    := $(call to_lower,$(debug))
USE_TSC     := $(call to_lower,$(use_tsc))
IS_HEADLESS := $(call to_lower,$(headless))

IS_MINGW := $(findstring mingw,$(shell $(CC) -dumpmachine))$(findstring w64,$(shell $(CC) -dumpmachine))

CFLAGS  := -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS := -pthread -lm

ifeq ($(IS_DEBUG),true)
    CFLAGS  += -g -O0 -fno-omit-frame-pointer -fsanitize=address,undefined
    LDFLAGS += -g -fsanitize=address,undefined
else
    CFLAGS  += -O3 -fstrict-overflow -fno-math-errno -flto=auto \
               -fno-stack-protector -fno-semantic-interposition -fno-plt \
               -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident
    LDFLAGS += -Wl,-rpath,'$$ORIGIN' -Wl,--gc-sections -Wl,--build-id=none -flto=auto -s
endif

ifeq ($(USE_TSC),true)
    CFLAGS += -DUSE_TSC_CLOCK
endif

ifeq ($(IS_NATIVE),true)
    CFLAGS += -march=native
endif

ifeq ($(USE_OPENMP),true)
    CFLAGS  += -fopenmp
    LDFLAGS += -fopenmp
endif

SRC_DIRS := . parse playback synth third_party
SRCS     := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))

ifeq ($(IS_HEADLESS),false)
    CFLAGS += -DNOT_HEADLESS
    SRCS   += $(wildcard render/*.c) $(wildcard third_party/glad/src/*.c)

    ifeq ($(IS_MINGW),)
        LDFLAGS += -lglfw -lGL
    else
        ifeq ($(GLFW_WIN_DIR),)
            $(error GLFW_WIN_DIR not set. Stopping compilation)
        endif
        CFLAGS  += -I$(GLFW_WIN_DIR)/include
        LDFLAGS += -L$(GLFW_WIN_DIR)/lib-mingw-w64 -lglfw3 -mconsole -lopengl32 -lgdi32 -luser32 -lkernel32
    endif
endif

OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS   := $(OBJS:.o=.d)
TARGET := $(BUILD_DIR)/sharkmidi$(if $(IS_MINGW),.exe,)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf build/*.o build/parse build/playback build/synth build/render build/third_party
