# Makefile for aesdsocket - Cross-compilation compatible
# 	Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
#	Version 2 Code (this): https://chat.deepseek.com/share/v6z76kub49dzcpxmqa
#	  Comparison: https://chatgpt.com/share/697cea7e-2700-8007-8fa5-a7edea60a08d
#
# Usage:
#   make                # Build for host system (x86_64 on x86_64, ARM64 on ARM64)
#   make clean          # Clean build artifacts
#   make CROSS_COMPILE=aarch64-linux-gnu- # Cross-compile for ARM64
#
# Note: CROSS_COMPILE should include the trailing hyphen

# Default compiler and flags
CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -Wextra -g -O2
LDFLAGS ?=

# Source and target definitions
TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

# Default target (same as 'all')
.DEFAULT_GOAL := all

# Build everything
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) -lpthread

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	rm -f *.d *.gcno *.gcda *.gcov

# Phony targets
.PHONY: all clean install uninstall
