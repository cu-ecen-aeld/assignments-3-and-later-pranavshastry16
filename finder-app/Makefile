# Allow cross-compilation by setting CROSS_COMPILE on the command line
CC := $(CROSS_COMPILE)gcc

CFLAGS := -Wall -Wextra -Werror -std=c11

TARGET := writer
SRCS := writer.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

