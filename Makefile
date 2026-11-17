CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS = -pthread

.PHONY: all clean

all: switch host

switch: switch.o port.o shared_frame.o frame.o mac_table.o admission.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

switch.o port.o admission.o: port.h shared_frame.h sync.h mac_table.h
switch.o admission.o: admission.h
mac_table.o: mac_table.h sync.h frame.h
shared_frame.o: shared_frame.h sync.h
switch.o host.o port.o shared_frame.o frame.o admission.o: frame.h

clean:
	rm -f switch host *.o
