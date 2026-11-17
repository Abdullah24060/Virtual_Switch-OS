CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS = -pthread

.PHONY: all clean

all: switch host

switch: switch.o port.o shared_frame.o frame.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

switch.o port.o: port.h shared_frame.h sync.h
shared_frame.o: shared_frame.h sync.h
switch.o host.o port.o shared_frame.o frame.o: frame.h

clean:
	rm -f switch host *.o
