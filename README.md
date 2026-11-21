# Virtual Switch

Operating Systems, Fall 2026 - Project

A software network switch in C. The switch is a multithreaded TCP server on Linux.
Each host is a separate process that connects to one switch port and sends frames
to other hosts. The switch learns which MAC is on which port and forwards frames
only where they need to go. Unknown destinations and broadcasts go to every port.

## Build

    make          builds switch, host, swctl and test
    make tsan     builds switch_tsan with ThreadSanitizer
    make clean    removes the programs

All files build with `gcc -std=c11 -Wall -Wextra -Werror -g -O0`.

## Run

Start the switch (the config file is optional):

    ./switch <tcp_port> <num_ports> [config_file]
    ./switch 9000 4
    ./switch 9000 6 switch.conf

Start hosts (each one in its own terminal):

    ./host <switch_ip> <tcp_port> <mac>
    ./host 127.0.0.1 9000 00:00:00:00:00:0a
    ./host 127.0.0.1 9000 00:00:00:00:00:0b

In a host, type a line to send a frame:

    00:00:00:00:00:0b hello     send to one host
    all hello                   broadcast to everyone
    prio 0                      next frames have priority 0 (0 high, 1 normal, 2 low)
    vlan 20                     next frames carry vlan 20 (matters on the trunk port)

Received frames are printed as:

    from 00:00:00:00:00:0A vlan 1 prio 1: hello

Admin tool (the switch must be running):

    ./swctl show mac          list learned MACs
    ./swctl show ports        state, queue and counters for each port
    ./swctl clear mac         empty the MAC table
    ./swctl stats             counters from shared memory
    ./swctl stats --watch     live counters, refresh every second
    ./swctl shutdown <port>   disconnect the host and keep the port down
    ./swctl up <port>         bring a down port back
    ./swctl vlan <port> <vlan>          move a port to a VLAN (1-4094)
    ./swctl mirror <port> <sniffer_port>   copy all traffic in and out of <port>
    ./swctl mirror <port> off
    ./swctl trunk <port>      make <port> the trunk (trunk off: no trunk)
    ./swctl rate <port> <n>   limit <port> to n frames per second (0 = no limit)

swctl talks to the switch over the UNIX socket `/tmp/vswitch.sock`.
`stats` reads the shared memory `/vswitch_stats` directly.

Signals:

    kill -USR1 <pid>    dump the MAC table to mac_dump.txt
    kill -HUP <pid>     reload the config file (a bad file is rejected, old settings stay)
    Ctrl+C or kill -INT <pid>    clean shutdown

Unit test for the frame header and MAC table:

    ./test

Log messages go to switch.log (and the terminal) with timestamps.

## Config file

One setting per line, `#` starts a comment. See `switch.conf`.

| Setting | Default | Meaning |
|---------|---------|---------|
| rate <n> | 0 | frames per second each port may send, 0 = unlimited |
| weights <h> <n> <l> | 5 3 1 | weighted round robin for the 3 priority queues |
| max_wait_ms <n> | 200 | a low/normal frame waiting this long is sent next (once per round) |
| aging <n> | 60 | seconds before an unused MAC is removed |
| port_max_macs <n> | 8 | port security: more MACs shuts the port down |
| wait_seconds <n> | 5 | how long an extra host waits for a free port |
| mac_table_size <n> | 1024 | most MACs in the table (startup only) |
| log_file <path> | switch.log | log file (startup only) |
| trunk <port> | -1 | the trunk port, -1 = none |
| vlan <port> <vlan> | 1 | VLAN of an access port |

SIGHUP applies everything except mac_table_size and log_file. Reload also
resets VLAN, trunk and rate changes made with swctl to what the file says.

## Behaviour notes

- Access ports: the switch sets the frame's vlan field to the port's VLAN.
  Trunk port: frames keep their vlan field; a frame with vlan outside 1-4094 is dropped.
- A frame only goes to ports in its VLAN, plus the trunk.
- Port security does not apply to the trunk (it carries many MACs).
  A port that breaks it is shut down and stays down until `swctl up`.
- Mirroring copies frames received on the port and frames sent out of it.
  Copies go into the sniffer's queue and are dropped if it is full, so the
  mirrored port never slows down.
- Shutting a port down drops the frames still in its queue.

## Frame format

Every frame is a 17-byte header followed by the payload.
length and vlan are in network byte order.

| Field    | Size    | Offset |
|----------|---------|--------|
| length   | 2 bytes | 0      |
| src      | 6 bytes | 2      |
| dst      | 6 bytes | 8      |
| vlan     | 2 bytes | 14     |
| priority | 1 byte  | 16     |

## Files

| File        | What it does |
|-------------|--------------|
| frame.c/h   | header serialize/deserialize, full reads and writes, reference-counted frames |
| mac_table.c/h | MAC table with one lock per bucket, aging, LRU eviction, per-port count |
| switch.c    | the switch: ports, queues, threads, forwarding, port semaphore, admin and signal threads |
| shm_stats.c/h | per-port counters in POSIX shared memory with a process-shared mutex |
| logger.c/h  | log queue and logger thread that writes timestamped lines |
| config.c/h  | config file parser |
| switch.conf | example config file |
| swctl.c     | admin tool |
| host.c      | test host that sends and receives frames |
| test.c      | unit test for frame and MAC table |

## Fixed limits

| Limit | Value | Where |
|-------|-------|-------|
| queue size per priority per port | 64 frames | switch.c `QUEUE_SIZE` |
| ports | 64 | shm_stats.h `MAX_PORTS` |
| token refill tick | 100 ms | switch.c `TICK_MS` |

## Progress

Tier 1 (Core):

- [x] 1. Wire protocol: partial reads/writes, mid-frame disconnect, SIGPIPE ignored
- [x] 2. No head-of-line blocking: bounded queue and sender thread per port, drops counted
- [x] 3. Shared broadcast frames with reference counts
- [x] 4. MAC table: striped locks, aging, MAC moved, LRU size limit, port security
- [x] 5. Port limit with a counting semaphore and sem_timedwait, "switch full" reply

Tier 2 (Advanced):

- [x] 6. QoS priority queues with weighted round robin
- [x] 7. Rate limiting with a token bucket
- [x] 8. VLANs and a trunk port
- [x] 9. Port mirroring
- [x] 10. Live reconfiguration
- [x] 11. swctl admin tool and shared memory stats
- [x] 12. Signals with sigwait (SIGINT, SIGHUP, SIGUSR1)
- [x] 13. Logger thread

Tier 3 (Bonus):

- [ ] Two switches linked by pipes
- [ ] Supervisor
- [ ] Benchmark
