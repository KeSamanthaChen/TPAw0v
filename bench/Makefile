# SPDX-License-Identifier: MIT
# Copyright 2023 Alexander Zuepke
#
# Makefile
#
# azuepke, 2023-12-25: initial

# cross-compiler for ARM64 Linux
#CROSS=aarch64-linux-gnu-
# cross-compiler for RISC-V 64-bit Linux
#CROSS=riscv64-linux-gnu-

# cross toolchain
LD = $(CROSS)gcc
CC = $(CROSS)gcc -c
DEPCC = $(CROSS)gcc -M
STRIP = $(CROSS)strip

# set VERBOSE to disable suppressing of compiler commandlines
ifeq ("$(VERBOSE)", "")
Q=@
endif

# default build ID
ifeq ("$(BUILDID)", "")
BUILDID := $(USER)@$(shell hostname) $(shell date +'%Y-%m-%d %H:%M:%S') git:$(shell git log -n1 --format='%h')
endif

CFLAGS = -W -Wall -Wextra -Werror -DBUILDID="\"$(BUILDID)\""

ifeq ("$(DEBUG)", "yes")
CFLAGS += -Og -g3
# override strip for debug builds
STRIP = touch --no-create
else
CFLAGS += -O2 -DNDEBUG
endif

# static binaries version
ifeq ("$(STATIC)", "yes")
LDFLAGS += -static
endif

MODS = bench
OBJS = $(addsuffix .o,$(MODS))
DEPS = $(addsuffix .d,$(MODS))

.PHONY: all clean distclean zap

all: bench

bench: $(addsuffix .o,$(MODS))
	@echo "  LINK  $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $^
	$(Q)$(STRIP) $@

clean:
	-$(Q)rm -f $(OBJS) bench

distclean: clean
	-$(Q)rm -f $(DEPS)
	-$(Q)rm -rf __pycache__

zap: distclean
	@# no extra steps right now

.SUFFIXES: .c .o .d

%.o: %.c
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -o $@ $<

%.d: %.c
	@echo "  DEPCC $<"
	$(Q)$(DEPCC) $(CFLAGS) -MT $(notdir $(patsubst %.c,%.o,$<)) -MF $@ $<

# pull in dependencies
ifeq ($(filter clean,$(MAKECMDGOALS))$(filter distclean,$(MAKECMDGOALS))$(filter zap,$(MAKECMDGOALS)),)
-include $(DEPS)
endif
