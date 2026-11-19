CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS = -pthread
SWITCH_SRC = switch.c port.c shared_frame.c frame.c mac_table.c admission.c qos.c rate_timer.c

.PHONY: all clean

all: switch host

switch: $(SWITCH_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

switch.o port.o admission.o rate_timer.o: port.h qos.h rate_timer.h shared_frame.h sync.h mac_table.h frame.h
switch.o admission.o: admission.h
mac_table.o: mac_table.h sync.h frame.h
qos.o: qos.h shared_frame.h frame.h
shared_frame.o: shared_frame.h sync.h frame.h
host.o frame.o: frame.h

clean:
	rm -f switch host *.o
