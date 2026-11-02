# Makefile — AlphaTheta XDJ-AZ Shims & Runtime for Armbian Amlogic S905X
# Clean, modular C build with native AArch64 and cross-compile support.

CROSS_COMPILE ?= 
CC            ?= $(CROSS_COMPILE)gcc
CFLAGS        ?= -O2 -Wall -Wextra -fPIC -pthread -std=gnu11 -D_GNU_SOURCE
LDFLAGS       ?= -shared -ldl -lpthread

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

# Standalone controller bridges
BINARIES      := $(OUT_DIR)/flx6_controls

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

$(OUT_DIR)/libaudio_shim.so: $(SRC_DIR)/audio_shim.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lasound

# Performance Shims
$(OUT_DIR)/ximage_fast24.so: $(SRC_DIR)/ximage_fast24.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lX11 -lXext 2>/dev/null || $(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

$(OUT_DIR)/fractional_grid.so: $(SRC_DIR)/fractional_grid.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lm

$(OUT_DIR)/fractional_grid_span.so: $(SRC_DIR)/fractional_grid_span.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS) -lm

$(OUT_DIR)/sem_owner.so: $(SRC_DIR)/sem_owner.c
	$(CC) $(CFLAGS) -I$(INC_DIR) $< -o $@ $(LDFLAGS)

# FLX6 Hardware Bridge
$(OUT_DIR)/flx6_controls: $(SRC_DIR)/flx6_controls.c $(INC_DIR)/xdj_az_abi64.h
	$(CC) -O2 -Wall -Wextra -pthread -std=gnu11 -D_GNU_SOURCE -I$(INC_DIR) $< -o $@

# Tests
tests: $(OUT_DIR) $(TEST_BINARIES)

$(OUT_DIR)/test_crc16: $(TEST_DIR)/test_crc16.c $(INC_DIR)/subucom_protocol.h
	$(CC) -O2 -Wall -I$(INC_DIR) $< -o $@

$(OUT_DIR)/test_subucom: $(TEST_DIR)/test_subucom.c $(INC_DIR)/subucom_protocol.h
	$(CC) -O2 -Wall -I$(INC_DIR) $< -o $@

$(OUT_DIR)/test_flx6_mapping: $(TEST_DIR)/test_flx6_mapping.c $(INC_DIR)/subucom_protocol.h $(INC_DIR)/xdj_az_abi64.h
	$(CC) -O2 -Wall -I$(INC_DIR) $< -o $@

check: tests
	@echo "=== Running Automated Test Suite ==="
	@$(OUT_DIR)/test_crc16
	@$(OUT_DIR)/test_subucom
	@$(OUT_DIR)/test_flx6_mapping
	@echo "=== All Tests Passed! ==="

clean:
	rm -rf $(OUT_DIR)

.PHONY: all tests check clean
