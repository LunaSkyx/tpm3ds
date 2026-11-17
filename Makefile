#---------------------------------------------------------------------------------
# tpm3ds - New 3DS build. One command: `make` (devkitPro shell).
#   make        -> tpm3ds.3dsx (+ .elf with symbols for addr2line)
#   make clean
# Host tests (no devkitPro needed): make -f Makefile.host test
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET   := tpm3ds
BUILD    := build
# 3DS build = core + shared pal + 3DS backend. File basenames must be unique.
SOURCES  := source source/core source/core/hle source/core/audio source/core/video source/core/render \n            source/gen source/pal/common source/pal/3ds
INCLUDES := source source/gen

APP_TITLE       := The Phantom Menace 3DS
APP_DESCRIPTION := Native port (bring your own game files)
APP_AUTHOR      := tpm3ds
# 48x48 icon, drawn by tools/cia/make_assets.py (relative: the build runs inside build/)
export APP_ICON := ../assets/icon.png

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -O3 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS   += $(INCLUDE) -D__3DS__ -std=gnu11 -DTPM_GMEM_MASKED $(TPM_DEFS)
# TPM_DEFS: extra defines for diagnostic builds, e.g. make TPM_DEFS=-DTPM_FN_TIMERS (rebuild all: rm -rf build)
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--wrap=svcSendSyncRequest -Wl,--wrap=svcWaitSynchronization -Wl,--wrap=svcWaitSynchronizationN -Wl,--wrap=svcArbitrateAddress
LIBS     := -lcitro3d -lctru -lm
LIBDIRS  := $(CTRULIB)

ifneq ($(BUILD),$(lastword $(notdir $(CURDIR))))
#---------------------------------------------------------------------------------
# All project paths are relative to $(BUILD): the project lives under a path
# with spaces, which make cannot handle in absolute VPATH/-I/-f arguments.
export OUTPUT   := ../$(TARGET)
export TOPDIR   := ..
export VPATH    := $(foreach dir,$(SOURCES),../$(dir))
export DEPSDIR  := .
CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD       := $(CC)
PICAFILES       := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
export OFILES_SOURCES := $(CFILES:.c=.o)
export HFILES   := $(PICAFILES:.v.pica=_shbin.h)
export OFILES   := $(PICAFILES:.v.pica=.shbin.o) $(OFILES_SOURCES)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I../$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I.
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS  := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh

.PHONY: all clean $(BUILD)
all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f ../Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf  : $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

.PRECIOUS : %.shbin
%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

# Lifted code: -O2 (measured on 3DS/Azahar 2026-11: see bench/o2_bench_log.txt).
wmain_gen_%.o bink_gen_%.o imuse_gen_%.o lifted_modules.o: CFLAGS += -O2 -w

-include $(DEPENDS)
endif
