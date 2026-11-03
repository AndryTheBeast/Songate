# Songate · tools/clicktest: the console's side of the session 10 test. Dev only: never shipped. `make` here gives clicktest.nro.
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := clicktest
BUILD    := build
SOURCES  := .
DATA     :=
INCLUDES :=
APP_TITLE := clicktest
APP_AUTHOR := Songate dev
APP_VERSION := 1

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ $(INCLUDE) $(shell $(PORTLIBS)/bin/aarch64-none-elf-pkg-config --cflags sdl2)
CXXFLAGS := $(CFLAGS) -std=gnu++20
ASFLAGS  := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := $(shell $(PORTLIBS)/bin/aarch64-none-elf-pkg-config --libs sdl2) -lmbedcrypto -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CPPFILES := clicktest.cpp
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o)
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export BUILD_EXEFS_SRC := exefs_src
export APP_ICON := $(LIBNX)/default_icon.jpg
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp
.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
