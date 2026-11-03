# Makefile — AlphaTheta XDJ-AZ Shims & Runtime for Armbian Amlogic S905X
# Clean, modular C build with native AArch64 and cross-compile support.

CROSS_COMPILE ?= 
CC            ?= $(CROSS_COMPILE)gcc
CFLAGS        ?= -O2 -Wall -Wextra -fPIC -pthread -std=gnu11 -D_GNU_SOURCE
LDFLAGS       ?= -shared -ldl -lpthread -lrt -lm

INC_DIR       := include
SRC_DIR       := src
OUT_DIR       := out
TEST_DIR      := tests

# Shims required for hardware emulation
HW_SHIMS      := $(OUT_DIR)/libsubucom_emu.so \
                 $(OUT_DIR)/libproc_usb_shim.so \
                 $(OUT_DIR)/libenv_emu_shim.so \
                 $(OUT_DIR)/libnet_shim.so \
                 $(OUT_DIR)/libcrashcatch.so \
                 $(OUT_DIR)/libtouch_shim.so \
                 $(OUT_DIR)/libaudio_shim.so

# Shims required for graphics and performance optimization
OPT_SHIMS     := $(OUT_DIR)/ximage_fast24.so \
                 $(OUT_DIR)/fractional_grid.so \
                 $(OUT_DIR)/fractional_grid_span.so \
                 $(OUT_DIR)/sem_owner.so

# Standalone controller bridges & input daemons
BINARIES      := $(OUT_DIR)/flx6_controls \
                 $(OUT_DIR)/vnc_touch_bridge

# Test binaries
TEST_BINARIES := $(OUT_DIR)/test_crc16 \
                 $(OUT_DIR)/test_subucom \
                 $(OUT_DIR)/test_flx6_mapping

ALL_TARGETS   := $(HW_SHIMS) $(OPT_SHIMS) $(BINARIES)

all: $(OUT_DIR) $(ALL_TARGETS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Hardware Shims
$(OUT_DIR)/libsubucom_emu.so: $(SRC_DIR)/subucom_emu.c $(INC_DIR)/xdj_az_abi64.h $(INC_DIR)/subucom_protocol.h
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/libproc_usb_shim.so: $(SRC_DIR)/proc_usb_shim.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/libenv_emu_shim.so: $(SRC_DIR)/env_emu_shim.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/libnet_shim.so: $(SRC_DIR)/net_shim.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/libcrashcatch.so: $(SRC_DIR)/crashcatch.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/libtouch_shim.so: $(SRC_DIR)/touch_shim.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lm

DSP_SRCS      := $(SRC_DIR)/dsp/cfx_filter.c \
                 $(SRC_DIR)/dsp/cfx_crush.c \
                 $(SRC_DIR)/dsp/cfx_dubecho.c \
                 $(SRC_DIR)/dsp/cfx_noise.c \
                 $(SRC_DIR)/dsp/cfx_space.c \
                 $(SRC_DIR)/dsp/cfx_sweep.c \
                 $(SRC_DIR)/dsp/cfx_manager.c \
                 $(SRC_DIR)/dsp/channel_eq.c \
                 $(SRC_DIR)/dsp/channel_eq_tables.c \
                 $(SRC_DIR)/dsp/deck_mix.c \
                 $(SRC_DIR)/dsp/dsp_graph.c \
                 $(SRC_DIR)/dsp/beat_fx.c \
                 $(SRC_DIR)/dsp/beat_echo.c \
                 $(SRC_DIR)/dsp/beat_manager.c \
                 $(SRC_DIR)/dsp/beat_grid.c \
                 $(SRC_DIR)/dsp/beat_quantize.c \
                 $(SRC_DIR)/dsp/beat_control.c \
                 $(SRC_DIR)/dsp/dsp_control.c \
                 $(SRC_DIR)/dsp/headphone_dsp.c

DSP_OPT_FLAGS ?= -O3 -fno-math-errno

$(OUT_DIR)/libaudio_shim.so: $(SRC_DIR)/audio_shim.c $(DSP_SRCS)
	$(CC) $(CFLAGS) $(DSP_OPT_FLAGS) -I$(INC_DIR) $^ -o $@ $(LDFLAGS) -lasound

# Performance Shims
$(OUT_DIR)/ximage_fast24.so: $(SRC_DIR)/ximage_fast24.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lX11 -lXext 2>/dev/null || $(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/fractional_grid.so: $(SRC_DIR)/fractional_grid.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lm

$(OUT_DIR)/fractional_grid_span.so: $(SRC_DIR)/fractional_grid_span.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lm

$(OUT_DIR)/sem_owner.so: $(SRC_DIR)/sem_owner.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

# FLX6 Hardware Bridge (Modular ctrl_map + map_flx6 + flx6_bridge + padmode + vu + led)
FLX6_SRCS     := $(SRC_DIR)/flx6_main.c \
                 $(SRC_DIR)/map_flx6.c \
                 $(SRC_DIR)/ctrl_map.c \
                 $(SRC_DIR)/flx6_bridge.c \
                 $(SRC_DIR)/padmode_zone.c \
                 $(SRC_DIR)/rbp_vu.c \
                 $(SRC_DIR)/rbp_led.c

$(OUT_DIR)/flx6_controls: $(FLX6_SRCS) $(INC_DIR)/ctrl_map.h $(INC_DIR)/flx6_bridge.h $(INC_DIR)/padmode_zone.h $(INC_DIR)/rbp_vu.h $(INC_DIR)/rbp_led.h $(INC_DIR)/xdj_az_abi64.h $(INC_DIR)/xdj_az_shm.h
	$(CC) -O2 -Wall -Wextra -pthread -std=gnu11 -D_GNU_SOURCE -I$(INC_DIR) $(FLX6_SRCS) -o $@ -lrt -lm

# VNC / Web Remote Real-Time Touch Bridge
$(OUT_DIR)/vnc_touch_bridge: $(SRC_DIR)/vnc_touch_bridge.c
	$(CC) -O2 -Wall -Wextra -pthread -std=gnu11 -D_GNU_SOURCE $< -o $@ -lX11 -lXtst

# Tests
tests: $(OUT_DIR) $(TEST_BINARIES)

$(OUT_DIR)/test_crc16: $(TEST_DIR)/test_crc16.c $(INC_DIR)/subucom_protocol.h
	$(CC) -O2 -Wall -I$(INC_DIR) $< -o $@

$(OUT_DIR)/test_subucom: $(TEST_DIR)/test_subucom.c $(INC_DIR)/subucom_protocol.h
	$(CC) -O2 -Wall -I$(INC_DIR) $< -o $@

TEST_FLX6_SRCS := $(TEST_DIR)/test_flx6_mapping.c \
                  $(SRC_DIR)/map_flx6.c \
                  $(SRC_DIR)/ctrl_map.c \
                  $(SRC_DIR)/flx6_bridge.c \
                  $(SRC_DIR)/padmode_zone.c \
                  $(SRC_DIR)/rbp_vu.c \
                  $(SRC_DIR)/rbp_led.c

$(OUT_DIR)/test_flx6_mapping: $(TEST_FLX6_SRCS) $(INC_DIR)/ctrl_map.h $(INC_DIR)/flx6_bridge.h $(INC_DIR)/padmode_zone.h $(INC_DIR)/rbp_vu.h $(INC_DIR)/rbp_led.h $(INC_DIR)/xdj_az_abi64.h
	$(CC) -O2 -Wall -pthread -I$(INC_DIR) $(TEST_FLX6_SRCS) -o $@ -lrt -lm

check: tests
	@echo "=== Running Automated Test Suite ==="
	@$(OUT_DIR)/test_crc16
	@$(OUT_DIR)/test_subucom
	@$(OUT_DIR)/test_flx6_mapping
	@echo "=== All Tests Passed! ==="

clean:
	rm -rf $(OUT_DIR)

.PHONY: all tests check clean
