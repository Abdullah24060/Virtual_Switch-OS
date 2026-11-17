# Virtual Switch — Operating Systems Fall 2026

A software network switch written in C for Linux.

## Progress

Day 1, Parts 1 and 2 are complete. Frames use the manual's exact 17-byte header
and up to 1500 payload bytes, with partial reads/writes and disconnect handling.
Each connected host has a receiver thread and a sender thread. Its output queue
holds 64 frame pointers; when full, new frames for that port are dropped and counted.
Broadcasts share one frame with a mutex-protected reference count.

Frames currently go to every other connected host. MAC learning and timed
semaphore admission come in Part 3. At this stage, an extra connection is closed
immediately when all ports are occupied. VLAN values and priorities are preserved;
VLAN isolation and priority scheduling come in Tier 2. There is no config file yet.

## Build and run

Requires GCC and Make. The Makefile uses the course flags:
`-std=c11 -Wall -Wextra -Werror -g -O0`, with `-pthread` for threads.

```bash
make
./switch 9000 8
```

Start hosts in separate terminals, using a different source MAC for each:

```bash
./host 127.0.0.1 9000 02:00:00:00:00:0a
./host 127.0.0.1 9000 02:00:00:00:00:0b
```

Arguments: `./switch <tcp_port> <num_ports>` and
`./host <switch_ip> <tcp_port> <mac>`. Use a numeric IPv4 address.

Type `<destination_mac> <vlan> <priority> [payload]` in a host:

```text
ff:ff:ff:ff:ff:ff 10 0 testing
```

Other hosts display the frame, with its payload in hexadecimal. The sender gets
no echo. VLANs are 1–4094; priorities are 0 (high), 1 (normal), or 2 (low). The
source MAC comes from the host argument and the payload length is calculated.
Ctrl-D on empty host input exits that host. The switch prints its input, output
and drop counters when it disconnects.

Ctrl-C stops the switch cleanly: a `sigwait` thread wakes main, sockets are shut
down, all workers are joined, queued references are released and resources freed.
`make clean` removes generated programs and object files.
