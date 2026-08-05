TARGET = aesdsocket
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
CFLAGS += -pthread
LDFLAGS ?=
LDFLAGS += -pthread

all: $(TARGET)

default: all

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) aesdsocket.c -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
