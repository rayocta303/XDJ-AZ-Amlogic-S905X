# Makefile for libaz_patch_mod.so
# AlphaTheta XDJ-AZ EP147 Runtime Patch & Mod Library

CC            ?= gcc
CFLAGS        ?= -O2 -Wall -Wextra -fPIC -pthread -std=gnu11 -D_GNU_SOURCE
LDFLAGS       ?= -shared -ldl -lpthread

TARGET        := libaz_patch_mod.so
SRCS          := az_patch_mod.c \
                 patch_beat_fx.c \
                 patch_mods.c \
                 patch_update_hide.c \
                 patch_spin_back.c \
                 patch_beat_jump.c \
                 patch_streaming_key.c \
                 patch_wave_zoom.c

HDRS          := az_linux_compat.h \
                 az_patch_core.h \
                 patch_beat_fx.h \
                 patch_mods.h \
                 patch_update_hide.h \
                 patch_spin_back.h \
                 patch_beat_jump.h \
                 patch_streaming_key.h \
                 patch_wave_zoom.h

OBJS          := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
