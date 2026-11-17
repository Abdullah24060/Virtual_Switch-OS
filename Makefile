CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0

.PHONY: all clean

all: switch host

switch: switch.o frame.o
	$(CC) $(CFLAGS) -o $@ $^

host: host.o frame.o
	$(CC) $(CFLAGS) -o $@ $^

switch.o host.o frame.o: frame.h

clean:
	rm -f switch host *.o
