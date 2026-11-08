# SPDX-License-Identifier: GPL-3.0-only
# Makefile for AZ Mods Menu (azmod.so)

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -fPIC -shared -fexceptions -fasynchronous-unwind-tables \
          -Iazmod -Icommon -I/usr/include/freetype2 \
          -DAZMOD_RELEASE -D_GNU_SOURCE

LDFLAGS ?= -shared -ldl -lpthread -lX11 -lXext -lfreetype -lm

SRCS := azmod/azmod.c azmod/menu.c azmod/text.c azmod/azconf.c
TARGET := azmod.so

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

