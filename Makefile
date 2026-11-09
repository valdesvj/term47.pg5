######################################
# TERM47 - DMCP5 program for SwissMicros DM42n: a VT102 text terminal over USB
# Based on the SwissMicros DMCP5_SDK Makefile (BSD-3), as Chess 47 and NAV47
######################################
TARGET = TERM47
# make                 TERM47.pg5 (needs DMCP5 3.58: the serial console, IFC 18)
# make test            PC tests: the terminal and the link's escaping (test/test_vt.c)
# make compare         real programs (ls top less vi lynx w3m man) through TERM47's terminal and pyte
# make sim             the PC simulator (build/sim), its serial console on a pseudo-terminal: test/run_sim.py
# make fonts           src/fonts.h again from the X11 misc-fixed fonts (tools/mkfont.py)

ifdef DEBUG
DEBUG = 1
endif

BUILD_DIR = build
PYTHON ?= python3

######################################
# Sources
######################################
C_INCLUDES += -Idmcp -Isrc
C_SOURCES  += dmcp/sys/pgm_syscalls.c
ASM_SOURCES = dmcp/startup_pgm.s
C_SOURCES  += src/main.c src/vt.c src/proto.c src/link.c
C_DEFS     += -DPLATFORM_IFC_VER=18

#######################################
# Toolchain
#######################################
CC      = arm-none-eabi-gcc
AS      = arm-none-eabi-gcc -x assembler-with-cpp
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

C_DEFS  += -D__weak="__attribute__((weak))" -D__packed="__attribute__((__packed__))"
CPUFLAGS += -mthumb -march=armv7e-m -mfloat-abi=hard -mfpu=fpv4-sp-d16

CFLAGS  = -std=gnu11 $(CPUFLAGS) $(C_DEFS) $(C_INCLUDES) -Wall -fdata-sections -ffunction-sections
CFLAGS += -Wno-misleading-indentation
ifeq ($(DEBUG), 1)
CFLAGS += -O0 -g -DDEBUG
else
CFLAGS += -O2 -g
endif
CFLAGS += -MD -MP -MF .dep/$(@F).d

LDSCRIPT = stm32_program.ld
LDFLAGS  = $(CPUFLAGS) -T$(LDSCRIPT) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
           -Wl,--wrap=_malloc_r -g

all: $(BUILD_DIR)/$(TARGET).pg5

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# QSPI CRC is compiled into the program header: if it changes, rebuild once.
$(BUILD_DIR)/$(TARGET).pg5: $(BUILD_DIR)/$(TARGET).elf tools/pg5tool.py
	$(OBJCOPY) --remove-section .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_flash.bin
	$(OBJCOPY) --only-section   .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_qspi.bin
	@if ! $(PYTHON) tools/pg5tool.py qspi $(BUILD_DIR)/$(TARGET)_qspi.bin src/qspi_crc.h; then \
	   rm -f $(BUILD_DIR)/pgm_syscalls.o $<; $(MAKE) --no-print-directory $@; exit $$?; fi
	$(PYTHON) tools/pg5tool.py pg5 $(BUILD_DIR)/$(TARGET)_flash.bin $@
	$(SIZE) $<

$(BUILD_DIR):
	mkdir -p $@

# PC tests
test: | $(BUILD_DIR)
	gcc -std=gnu11 -O1 -g -Wall -Isrc -o $(BUILD_DIR)/test_vt test/test_vt.c src/vt.c src/proto.c && $(BUILD_DIR)/test_vt
	$(PYTHON) test/test_host.py

compare: | $(BUILD_DIR)
	gcc -std=gnu11 -O2 -Wall -Isrc -o $(BUILD_DIR)/vt_dump test/vt_dump.c src/vt.c
	$(PYTHON) test/compare_vt.py 66 26 vt102 && $(PYTHON) test/compare_vt.py 80 26 vt102 && \
	$(PYTHON) test/compare_vt.py 50 16 vt220 && $(PYTHON) test/compare_vt.py 40 10 vt100

fonts:
	$(PYTHON) tools/mkfont.py > src/fonts.h

clean:
	-rm -fR .dep $(BUILD_DIR)

-include $(shell mkdir .dep 2>/dev/null) $(wildcard .dep/*)

.PHONY: clean all test compare fonts sim

# the PC simulator: the real main.c, vt.c, link.c against a fake DMCP layer (test/sim); its serial console is the
# pseudo-terminal named in T47_PORT (test/run_sim.py makes one and runs tools/term47host.py on the other end)
sim:
	mkdir -p $(BUILD_DIR)
	gcc -O2 -w '-D__packed=__attribute__((__packed__))' '-D__weak=__attribute__((weak))' -DTERM47_SIM \
	    -Itest/sim -Isrc -o $(BUILD_DIR)/sim test/sim/sim.c test/sim/sercon_sim.c \
	    src/main.c src/vt.c src/proto.c src/link.c
