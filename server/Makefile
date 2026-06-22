TARGET = aesdsocket
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g

all: $(TARGET)

default: all

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) aesdsocket.c -o $(TARGET)

clean:
	rm -f $(TARGET)
