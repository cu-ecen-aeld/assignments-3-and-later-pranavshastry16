TARGET := aesdsocket
SRC := aesdsocket.c
OBJ := $(SRC:.c=.o)

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -O2 -DUSE_AESD_CHAR_DEVICE=1
LDFLAGS ?=

all: $(TARGET)

default: all

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all default clean

