CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS = -pthread
SWITCH_SRC = switch.c port.c shared_frame.c frame.c mac_table.c admission.c qos.c rate_timer.c options.c admin.c admin_protocol.c stats.c
SWCTL_SRC = swctl.c admin_protocol.c stats.c frame.c

.PHONY: all clean

all: switch host swctl

switch: $(SWITCH_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

swctl: $(SWCTL_SRC:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

switch.o port.o admission.o rate_timer.o admin.o: port.h qos.h rate_timer.h shared_frame.h sync.h mac_table.h frame.h stats.h
switch.o admission.o admin.o: admission.h
switch.o admin.o: admin.h
switch.o admin.o admin_protocol.o swctl.o: admin_protocol.h
swctl.o stats.o: stats.h
stats.o: sync.h
switch.o options.o: options.h
options.o: frame.h rate_timer.h
mac_table.o: mac_table.h sync.h frame.h
qos.o: qos.h shared_frame.h frame.h
shared_frame.o: shared_frame.h sync.h frame.h
host.o frame.o swctl.o: frame.h

clean:
	rm -f switch host swctl *.o
