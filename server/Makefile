# Makefile for aesdsocket - Cross-compilation compatible
#   Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
#   Version 2 Code (this): https://chat.deepseek.com/share/v6z76kub49dzcpxmqa
#     Comparison: https://chatgpt.com/share/697cea7e-2700-8007-8fa5-a7edea60a08d
#   Assignment 9 Revision: https://claude.ai/share/940b08c1-937e-4f59-9fe4-db34cee65a86
#	Correction: https://claude.ai/share/9a4a44e3-7fa6-4ed8-9f63-768d2319d8ad
#
# Usage:
#   make                              # Auto-detect aesd_ioctl.h, build for host
#   make clean                        # Clean build artifacts
#   make CROSS_COMPILE=aarch64-linux-gnu-   # Cross-compile for ARM64
#   make DRIVER_DIR=/path/to/driver   # Override driver tree location
#
# aesd_ioctl.h resolution (in priority order):
#   1. $(DRIVER_DIR)/aesd_ioctl.h  – explicit override
#   2. ../aesd-char-driver/aesd_ioctl.h  – default relative path
#   3. Inline fallback in aesdsocket.c  – when neither is found
#
# Note: CROSS_COMPILE should include the trailing hyphen.

CROSS_COMPILE ?=
CC            ?= $(CROSS_COMPILE)gcc
CFLAGS        ?= -Wall -Werror -Wextra -g -O2
LDFLAGS       ?=

TARGET = aesdsocket
SRCS   = aesdsocket.c
OBJS   = $(SRCS:.c=.o)

# ---------------------------------------------------------------------------
# aesd_ioctl.h detection
#
# DRIVER_DIR can be set on the command line to point at any directory that
# contains aesd_ioctl.h.  The default is the sibling directory that the
# course repository layout produces when both repos are cloned side-by-side:
#
#   assignments/
#     server/            <- this Makefile lives here
#     aesd-char-driver/  <- driver tree lives here
#
# $(wildcard) expands to the path if the file exists, or to the empty string
# if it does not.  This makes the detection completely automatic.
# ---------------------------------------------------------------------------
DRIVER_DIR ?= ../aesd-char-driver
IOCTL_HEADER := $(wildcard $(DRIVER_DIR)/aesd_ioctl.h)

ifneq ($(IOCTL_HEADER),)
    # Header found: add the driver directory to the include path and define
    # the flag that switches aesdsocket.c from its inline fallback to the
    # real shared header.
    override CFLAGS += -I$(DRIVER_DIR) -DHAVE_AESD_IOCTL_H
    $(info aesd_ioctl.h found at $(IOCTL_HEADER) — using project header)
else
    # Header not found: the inline fallback definitions in aesdsocket.c will
    # be used automatically.  This is safe for standalone builds and CI
    # environments that do not have the driver tree checked out.
    $(info aesd_ioctl.h not found in $(DRIVER_DIR) — using inline fallback)
endif

# ---------------------------------------------------------------------------
# Standard targets
# ---------------------------------------------------------------------------
.DEFAULT_GOAL := all

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f *.d *.gcno *.gcda *.gcov

.PHONY: all clean
