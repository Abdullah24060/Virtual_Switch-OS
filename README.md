# Virtual Switch — Operating Systems Fall 2026

A software network switch written in C for Linux.

## Progress

Day 1, Part 1 is complete: TCP connections and the fixed 17-byte frame header,
with payloads up to 1500 bytes. Partial reads/writes, invalid frames and host
disconnections are handled.

For now, the switch reads and prints frames from one host at a time. Forwarding,
port threads and MAC learning come next. `num_ports` currently sets the listen
backlog; the logical port limit will be added later. There is no config file yet.

## Build and run

Requires GCC and Make. The Makefile uses the course flags:
`-std=c11 -Wall -Wextra -Werror -g -O0`.

```bash
make
./switch 9000 8
```

In another terminal:

```bash
./host 127.0.0.1 9000 02:00:00:00:00:0a
```

The arguments are `./switch <tcp_port> <num_ports>` and
`./host <switch_ip> <tcp_port> <mac>`. Use a numeric IPv4 address and a MAC with
six colon-separated hexadecimal pairs.

Type messages in the host as `<destination_mac> <vlan> <priority> [payload]`:

```text
ff:ff:ff:ff:ff:ff 10 0 testing
```

VLANs are 1–4094. Priorities are 0 (high), 1 (normal), or 2 (low). The source MAC
comes from the host argument, and the payload length is calculated automatically.
The switch displays payloads in hexadecimal. Ctrl-D on empty host input exits
the host; Ctrl-C stops the switch. Use `make clean` to remove build files.

## Validation

The protocol checks and 20 integration tests passed during development.
Valgrind reported zero errors and no lost memory; the switch's libc output
buffer remained reachable when stopped with Ctrl-C. ThreadSanitizer reported
no warnings with ASLR disabled only for its test processes. This validates the
current single-threaded milestone; the threaded parts will be checked again.
