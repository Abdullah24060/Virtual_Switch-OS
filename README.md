# Virtual Switch — Operating Systems

A Linux switch written in C. The project builds three programs: `switch` runs the server, `host` sends and receives frames, and `swctl` controls the running switch.

## 1. Build and start the switch

Open four terminals in the project folder. On Linux Mint, run these commands in **Terminal 1** with GCC and Make installed:

```bash
make
./switch 9000 3
```

`9000` is the TCP port the hosts connect to. `3` is the number of host connections available. Wait for `listening`. With these defaults, all hosts use VLAN 1.

## 2. Connect two hosts and send a message

**Terminal 2 — Host A.** Start this one first so it gets switch port 1.

```bash
./host 127.0.0.1 9000 02:00:00:00:00:0a
```

**Terminal 3 — Host B.** Start this one next so it gets switch port 2.

```bash
./host 127.0.0.1 9000 02:00:00:00:00:0b
```

**In Host A**, type this line and press Enter. Host B should display `payload=hello everyone`.

```text
ff:ff:ff:ff:ff:ff 1 1 hello everyone
```

**In Host B**, reply directly to A. Host A should display `payload=hello A`.

```text
02:00:00:00:00:0a 1 1 hello A
```

Each line means `destination_MAC VLAN priority message`. The all-`ff` address broadcasts to the VLAN. Priority is `0` for high, `1` for normal or `2` for low. The host supplies its own source MAC and calculates the payload length.

## 3. Check the running switch

In **Terminal 4**, run:

```bash
./swctl show ports
./swctl show mac
./swctl stats
```

After the exchange, ports 1 and 2 should be active. The MAC table should show A on port 1 and B on port 2. Use `./swctl stats --watch` for live counters. Press Ctrl+C to leave the watch.

## 4. Run with a config file and reload it

The included `switch.conf` contains two lines:

```text
vlan 1 20
rate 50
```

This puts switch port 1 on VLAN 20. Ports 2 and 3 keep the default VLAN 1. Each port can send up to 50 frames per second.

1. Press **Ctrl+C in Terminal 1** to stop the first switch. Its hosts disconnect.
2. In **Terminal 1**, start a new switch using a copy of the config. Keep this terminal open. The last line saves the switch's process ID for reloading.

   ```bash
   cp switch.conf demo.conf
   ./switch 9000 3 --config demo.conf &
   switch_pid=$!
   ```

3. Wait for `listening`, then restart **Host A first and Host B second** using the commands in step 2. Run `./swctl show ports` in Terminal 4. Check for `vlan=20` on port 1, `vlan=1` on port 2 and `rate=50` on both.
4. In **Host A**, enter `ff:ff:ff:ff:ff:ff 20 1 hello VLAN 20`. Host B stays quiet because it belongs to VLAN 1.
5. In **Terminal 1**, add port 2 to VLAN 20 and tell the running switch to reread the file:

   ```bash
   echo 'vlan 2 20' >> demo.conf
   kill -HUP "$switch_pid"
   ```

6. After `config reloaded`, use `./swctl show ports` to confirm both ports show `vlan=20`. Send the same line from Host A again. **Host B now receives `hello VLAN 20`** on its existing connection.
7. To stop the background switch, run `kill -TERM "$switch_pid"` in Terminal 1.

### Config file format

Use one setting per line. Blank lines are allowed. A `#` starts a comment. Port numbers start at 1.

| Setting | Meaning |
| --- | --- |
| `vlan PORT VLAN` | Give an access port a VLAN from 1 to 4094. |
| `rate FPS` | Limit each port to 1–10000 frames per second. The default is 1000. |
| `trunk PORT` or `trunk off` | Choose one port to carry all VLANs, or turn the trunk off. |
| `mirror SOURCE SNIFFER` or `mirror off` | Copy the source port's incoming and outgoing traffic to a separate receive-only port, or turn mirroring off. The sniffer cannot be the trunk. |

## 5. Test VLANs, the trunk and port mirroring

Use this as a separate demo. It uses TCP port **9001**, so every host and admin command below selects that switch. Open the terminals in the project folder.

### Start the switch and connect the hosts

In **Terminal 1**, start the switch and leave it running:

```bash
make
./switch 9001 5 --vlan 1 10 --vlan 2 10 --vlan 3 20 --trunk 4
```

Wait for `listening`. Run **one command per terminal, in the order below**. After starting each host, wait for Terminal 1 to print its port number before starting the next.

| Terminal | Role | Command to run |
| --- | --- | --- |
| 2 | A · port 1 · VLAN 10 | `./host 127.0.0.1 9001 02:00:00:00:00:0a` |
| 3 | B · port 2 · VLAN 10 | `./host 127.0.0.1 9001 02:00:00:00:00:0b` |
| 4 | C · port 3 · VLAN 20 | `./host 127.0.0.1 9001 02:00:00:00:00:0c` |
| 5 | T · port 4 · trunk | `./host 127.0.0.1 9001 02:00:00:00:00:0d` |
| 6 | M · port 5 · future sniffer | `./host 127.0.0.1 9001 02:00:00:00:00:0e` |
| 7 | Admin | `./swctl --port 9001 show ports` |

Check that all five ports say `active` and port 4 says `role=trunk`. Port 5 starts as an ordinary port on VLAN 1. Leave all hosts connected for the following tests.

### Test A — VLAN isolation and trunk tags

An access port belongs to one VLAN. The trunk carries frames for both VLANs and keeps the VLAN number on each frame. Type each line **inside the named host** and press Enter. Only the listed receivers should print a new `received` line.

| Type in | Line to enter | Where to look |
| --- | --- | --- |
| A (Terminal 2) | `ff:ff:ff:ff:ff:ff 10 1 from A` | B and T receive `from A` with `vlan=10`. C stays quiet. |
| C (Terminal 4) | `ff:ff:ff:ff:ff:ff 20 1 from C` | T receives `from C` with `vlan=20`. A and B stay quiet. |
| T (Terminal 5) | `ff:ff:ff:ff:ff:ff 10 1 trunk to ten` | A and B receive `trunk to ten`. |
| T (Terminal 5) | `ff:ff:ff:ff:ff:ff 20 1 trunk to twenty` | Only C receives `trunk to twenty`. |
| A (Terminal 2) | `ff:ff:ff:ff:ff:ff 20 1 wrong tag` | Every receiver stays quiet. A's access port drops the frame because it expects VLAN 10. |

### Test B — Mirror A's traffic to M

In **Terminal 7**, enable the mirror and check the roles:

```bash
./swctl --port 9001 mirror 1 5
./swctl --port 9001 show ports
```

Port 1 should show `mirror=5`. Port 5 should show `role=sniffer`. M can now see a copy when A sends **or** receives a frame. Run the first two rows back to back so A's learned MAC is still fresh.

| Type in | Line to enter | Expected receivers |
| --- | --- | --- |
| A (Terminal 2) | `ff:ff:ff:ff:ff:ff 10 1 A is sending` | B and T receive the broadcast. M gets one mirror copy. This also teaches the switch A's MAC. |
| B (Terminal 3) | `02:00:00:00:00:0a 10 1 private to A` | A receives the direct message. M gets one copy because A received it. T and C stay quiet. |
| C (Terminal 4) | `ff:ff:ff:ff:ff:ff 20 1 unrelated traffic` | Only T receives it. M stays quiet because this frame never passes through A's port. |
| M (Terminal 6) | `ff:ff:ff:ff:ff:ff 10 1 sniffer speaking` | Every receiver stays quiet. The sniffer is receive-only. |

Run `./swctl --port 9001 stats` in **Terminal 7**. If you sent each row once, port 1 should show `mirrored=2`.

### Test C — Change a live VLAN, disconnect a host and reconnect it

1. In **Terminal 7**, run `./swctl --port 9001 vlan 2 20`. In **Host B**, enter `ff:ff:ff:ff:ff:ff 20 1 B moved`. **C and T** receive it. A and M stay quiet.
2. In **Terminal 7**, run `./swctl --port 9001 shutdown 2`, then `./swctl --port 9001 show ports`. Port 2 says `state=down` and Host B returns to its shell.
3. In **Terminal 7**, run `./swctl --port 9001 up 2`, then `./swctl --port 9001 show ports`. Port 2 says `state=free`. In **Terminal 3**, rerun B's host command from the setup table. Port 2 becomes `active`. Send the same `B moved` line again to confirm that C and T receive it.
4. With the hosts idle, run `./swctl --port 9001 clear mac`, then `./swctl --port 9001 show mac` in **Terminal 7**. Expect `MAC table (0 entries)`. Send another frame from B and check again to see its MAC on port 2, VLAN 20.
5. Press **Ctrl+C in Terminal 1** to stop this switch and disconnect its hosts.

## Other commands and options

| Command or option | Use |
| --- | --- |
| `./swctl shutdown 2` / `./swctl up 2` | Disconnect and disable port 2 / enable it for a new host connection. |
| `./swctl vlan 2 20` / `./swctl mirror 1 3` | Move port 2 to VLAN 20 / mirror port 1 to a sniffer on port 3. |
| `./swctl clear mac` | Clear learned MAC addresses. |
| `./swctl --port 9001 show ports` | Select a switch started on TCP port 9001. The default is 9000. |
| `--vlan PORT VLAN`, `--rate FPS`, `--trunk PORT`, `--mirror SOURCE SNIFFER` | Set these options directly when starting the switch. A config file can override them. |
| `--log PATH`, `--mac-dump PATH` | Change the default log (`switch.log`) or MAC dump (`mac_table.txt`). Use `kill -USR1 "$switch_pid"` while the background switch is running to write a dump. |

To change the MAC table size, MAC limit per port, aging time or wait for a free port, supply all four numbers before any flags. For example, `./switch 9000 3 1024 8 60 5` uses the defaults of 1024 entries, 8 MACs per port, 60 seconds and 5 seconds.

Use `make clean` to remove the compiled programs and object files.
