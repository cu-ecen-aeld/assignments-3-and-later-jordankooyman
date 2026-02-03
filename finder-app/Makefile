# Writer Makefile
# Jordan Kooyman
# 2026-01-19
# Initial outline generated using ChatGPT: https://chatgpt.com/share/696e7561-ec28-8007-9140-2e62cc26f0e3

# Toolchain configuration
CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

# Project settings
TARGET  := writer
SRC     := writer.c
OBJ     := $(SRC:.c=.o)

# Flags
CFLAGS  := -Wall -Wextra -O2
LDFLAGS := -static -static-libgcc

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Cleanup
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean

