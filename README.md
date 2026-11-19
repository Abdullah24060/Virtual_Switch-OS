# Virtual Switch — Operating Systems Fall 2026

A software network switch written in C for Linux. It supports Tier 1 and Tier 2,
including live administration, config reload, MAC dumps and timestamped logs.

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
- VLAN-aware MAC learning with 32 bucket locks, aging, exact LRU eviction and
  MAC moves. Known destinations receive unicast. Unknown destinations and
  broadcasts flood within the frame's VLAN and to the trunk. Frames addressed
  back to their incoming port are filtered.
- Per-port MAC limits: exceeding the limit shuts down that port. Its resources
  and learned entries are cleaned up before another connection can reuse it.
- A counting semaphore limits connected hosts. An extra host waits for a free
  port, then receives `switch full` and disconnects if the timeout expires.
- Clean Ctrl-C shutdown, with every thread joined and shared frame released.

MAC moves and security events appear in the switch terminal and in
`switch.log`. The trunk, access VLANs and mirror can change during a reload.

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
VLANs are 1–4094 and priorities are 0 (high), 1 (normal), or 2 (low). The host
shows printable ASCII as text (`payload=hello B`) and other bytes as hex
(`payload_hex=00ff`). Ctrl-D on empty input disconnects the host. The switch
prints its input, output, drop and mirror counters. `make clean` removes generated
programs and object files.

## VLANs, trunk and mirror

Port numbers are the one-based logical numbers printed when each host connects.
On a fresh switch, connections take the lowest free port in order.

| Option | Effect |
| --- | --- |
| `--vlan PORT VLAN_ID` | Sets the VLAN of a normal port. Repeat for other ports. The default is VLAN 1. |
| `--trunk PORT` | Allows one port to send and receive any valid VLAN tag. Tags are preserved. |
| `--mirror SOURCE SNIFFER` | Copies accepted ingress and successfully written egress traffic on one source port to a dedicated receive-only sniffer. A trunk can be the source. |

For example, ports 1 and 2 share VLAN 10, port 3 is on VLAN 20, port 4 is
the trunk and port 5 monitors port 1:

```bash
./switch 9000 5 --vlan 1 10 --vlan 2 10 --vlan 3 20 --trunk 4 --mirror 1 5
```

Use the access port's VLAN in its host input. A wrong tag is dropped before
MAC learning. A sniffer is excluded from ordinary forwarding and its own
input is discarded. It receives mirror copies with the original VLAN and
priority. Copies use the sniffer's bounded queues and rate limit. A full
sniffer queue drops the mirror copy while ordinary forwarding continues.
The source port's `mirrored` counter counts copies queued successfully.

## Live administration

`swctl` uses a UNIX socket in a private directory for your user. It selects
the switch on TCP port 9000 by default. For another instance, put
`--port TCP_PORT` before the command.

| Command | Effect |
| --- | --- |
| `./swctl show ports` | Show each port's state, role, access VLAN, mirror target and send rate. A zero VLAN or mirror value means no access VLAN or no mirror. |
| `./swctl show mac` | List learned MAC addresses with their VLAN and port. |
| `./swctl shutdown PORT` | Disconnect the host, discard queued frames and keep the slot disabled. |
| `./swctl up PORT` | Enable the slot so a host can connect again. |
| `./swctl vlan PORT VLAN` | Change an access port's VLAN on the existing connection. Drop old queued frames and forget that connection's learned MACs. |
| `./swctl mirror SOURCE SNIFFER` | Replace the mirror rule. The previous sniffer becomes a normal port again if the target changes. |
| `./swctl clear mac` | Empty the MAC table. Subsequent frames learn addresses again. |
| `./swctl stats` | Read the latest per-port counters from POSIX shared memory. |
| `./swctl stats --watch` | Refresh once per second. Press Ctrl+C to stop watching. |

The counters reset when a fresh host connects to a port. A VLAN or mirror
change discards affected queued frames. A write already in progress can
finish on its current connection.

Forwarding takes the read side of a shared lock, so receivers can still
operate concurrently. VLAN, mirror and MAC clear commands take the write
side. The forwarding lock comes before individual port locks or the MAC
structure lock. The admission table lock comes before a port lock during
start, shutdown and cleanup. Joins hold only the admission table lock.
Structural MAC changes take the structure lock before bucket locks; eviction
takes buckets in ascending order. Queue operations take the port lock before
frame reference locks. Counter publication takes the shared stats mutex last.
Socket writes and admin replies run outside those data locks.

## Config file, signals and logs

Start with `./switch 9000 3 --config switch.conf` to load a text config file.
Here is a file for three ports:

```text
# An access VLAN for each port
vlan 1 10
vlan 2 10
vlan 3 20
rate 100
```

Use one directive per line. Blank lines and `#` comments are allowed.

| Directive | Effect |
| --- | --- |
| `vlan PORT VLAN` | The VLAN a port uses in the access role. VLAN IDs are 1–4094. |
| `trunk PORT` or `trunk off` | Select or remove the trunk. |
| `mirror SOURCE SNIFFER` or `mirror off` | Select or remove a mirror. The sniffer must differ from the source and the trunk. |
| `rate FPS` | Set the send rate per port. The range is 1–10000 frames per second. |

The file overrides the startup defaults and command-line flags. If a
directive repeats, the last value is used. A reload builds a new configuration
from the original command line and the current file, so it can replace
settings changed with `swctl`. Connections stay up and disabled ports stay
disabled. A port whose VLAN or role changes discards its queued frames and
forgets its learned MACs. The TCP port, port count and positional limits
remain startup settings.

- `kill -HUP PID` reloads the configured file. A missing or invalid file is
  rejected and the running settings stay in place.
- `kill -USR1 PID` writes a snapshot of learned MACs, VLANs and ports to
  `mac_table.txt`, replacing any previous dump.
- Ctrl+C (`SIGINT`) or `kill -TERM PID` shuts down the switch, joins its
  threads and finishes writing queued log events.

`--log PATH` and `--mac-dump PATH` override `switch.log` and `mac_table.txt`.
The logger appends UTC timestamps to switch events such as connections, MAC
moves, port security, admin changes and signals. Worker threads enqueue
events. The logger thread writes them outside the queue lock. Under overload,
the bounded queue drops excess events and the logger reports the count.

## Limits

Defaults: **1024 VLAN/MAC entries**, **8 distinct learned MACs per port**,
**60-second source aging**, and a **5-second admission timeout**. Destination
lookups update LRU order but do not refresh source aging.

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
`--rate <frames_per_second>` to set the initial rate for all ports. Each port has
its own token bucket. Place flags after the required arguments and any optional
positional limits.
