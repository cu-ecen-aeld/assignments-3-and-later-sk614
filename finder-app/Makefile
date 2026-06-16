CC := $(CROSS_COMPILE)gcc
CFLAGS := -Wall -Werror
TARGET := writer
OBJS := writer.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

writer.o: writer.c
	$(CC) $(CFLAGS) -c writer.c -o writer.o

clean:
	rm -f $(TARGET) *.o
