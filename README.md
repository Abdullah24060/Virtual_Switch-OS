# Virtual Switch — Operating Systems Fall 2026

A software network switch written in C for Linux. Tier 1, priority scheduling
and rate limiting are implemented.

## What works

- The manual's 17-byte frame header and payloads up to 1500 bytes, including
  partial reads/writes and hosts disconnecting in the middle of a frame.
- A receiver and sender for each connected host, with separate high, normal and
  low output queues. Each queue holds 64 frames. Full queues count and drop new
  arrivals. Broadcast queues share one frame by pointer.
- Weighted round robin gives backlogged priority classes a 5:3:1 share. The
  oldest queue head takes precedence once it has waited 200 ms. Frames stay in
  arrival order within their class.
- A timer refills a one-token bucket for each port. A port can send immediately
  after connecting or sitting idle, then waits for a token between frames.
- MAC learning with 32 bucket locks, aging, exact LRU eviction and MAC moves.
  Known destinations receive unicast; unknown destinations and broadcasts flood
  every other port. Frames addressed back to their incoming port are filtered.
- Per-port MAC limits: exceeding the limit shuts down that port. Its resources
  and learned entries are cleaned up before another connection can reuse it.
- A counting semaphore limits connected hosts. An extra host waits for a free
  port, then receives `switch full` and disconnects if the timeout expires.
- Clean Ctrl-C shutdown, with every thread joined and shared frame released.

MAC moves and security events appear in the switch terminal. VLAN IDs are
carried in frames. VLAN isolation, administration and file logging are planned
for the remaining Tier 2 parts.

## Build and run

Requires GCC and Make. The build uses
`-std=c11 -Wall -Wextra -Werror -g -O0` and `-pthread`.

```bash
make
./switch 9000 8
```

Start each host in its own terminal, with a different source MAC:

```bash
./host 127.0.0.1 9000 02:00:00:00:00:0a
./host 127.0.0.1 9000 02:00:00:00:00:0b
```

Arguments are `./switch <tcp_port> <num_ports>` and
`./host <switch_ip> <tcp_port> <mac>`. Use a numeric IPv4 address.
Type `<destination_mac> <vlan> <priority> [payload]` in a host:

```text
ff:ff:ff:ff:ff:ff 1 1 hello everyone
02:00:00:00:00:0b 1 1 hello B
```

A host must send a frame before the switch can learn its MAC. The source MAC
comes from the host argument and the payload length is calculated automatically.
VLANs are 1–4094 and priorities are 0 (high), 1 (normal), or 2 (low). Received
payloads appear in hexadecimal. Ctrl-D on empty input disconnects the host;
the switch prints its input, output and drop counters. `make clean` removes
generated programs and object files.

## Limits

Defaults: **1024 MAC entries**, **8 learned MACs per port**, **60-second source
aging**, and a **5-second admission timeout**. Destination lookups update LRU
order but do not refresh source aging.

To override defaults, provide all four extra arguments:

```text
./switch <tcp_port> <num_ports> <mac_capacity> <macs_per_port> <age_seconds> <wait_seconds>
```

For example, this allows two hosts and uses shorter limits for a demonstration:

```bash
./switch 9000 2 16 4 10 3
```

Capacity accepts 1–1000000, MACs per port 1–65535, age 1–86400 seconds and wait
1–3600 seconds. TCP port and number of switch ports accept 1–65535.

The default send limit is **1,000 frames per second per port**. Add
`--rate <frames_per_second>` as the last two arguments to set the initial rate
for all ports. Each port has its own token bucket.
