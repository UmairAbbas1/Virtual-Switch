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

Start the switch:

    ./switch <tcp_port> <num_ports>
    ./switch 9000 4

Start hosts (each one in its own terminal):

    ./host <switch_ip> <tcp_port> <mac>
    ./host 127.0.0.1 9000 00:00:00:00:00:0a
    ./host 127.0.0.1 9000 00:00:00:00:00:0b

In a host, type a line to send a frame:

    00:00:00:00:00:0b hello     send to one host
    all hello                   broadcast to everyone

Received frames are printed as:

    from 00:00:00:00:00:0A vlan 1 prio 1: hello

Admin tool (the switch must be running):

    ./swctl show mac          list learned MACs
    ./swctl show ports        state, queue and counters for each port
    ./swctl clear mac         empty the MAC table
    ./swctl stats             counters from shared memory
    ./swctl stats --watch     live counters, refresh every second

swctl talks to the switch over the UNIX socket `/tmp/vswitch.sock`.
`stats` reads the shared memory `/vswitch_stats` directly.

Signals:

    kill -USR1 <pid>    dump the MAC table to mac_dump.txt
    kill -HUP <pid>     config reload (prints a message for now)
    Ctrl+C or kill -INT <pid>    clean shutdown

Unit test for the frame header and MAC table:

    ./test

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
| swctl.c     | admin tool |
| host.c      | test host that sends and receives frames |
| test.c      | unit test for frame and MAC table |

## Settings

These are fixed in the code for now:

| Setting          | Value | Where |
|------------------|-------|-------|
| queue size per port | 64 frames | switch.c `QUEUE_SIZE` |
| MACs per port (port security) | 8 | switch.c `PORT_MAX_MACS` |
| MAC table size   | 1024 | mac_table.h `MAX_ENTRIES` |
| MAC aging time   | 60 s | switch.c `AGE_SECONDS` |
| wait for free port | 5 s | switch.c `WAIT_SECONDS` |

## Progress

Tier 1 (Core):

- [x] 1. Wire protocol: partial reads/writes, mid-frame disconnect, SIGPIPE ignored
- [x] 2. No head-of-line blocking: bounded queue and sender thread per port, drops counted
- [x] 3. Shared broadcast frames with reference counts
- [x] 4. MAC table: striped locks, aging, MAC moved, LRU size limit, port security
- [x] 5. Port limit with a counting semaphore and sem_timedwait, "switch full" reply

Tier 2 (Advanced):

- [ ] 6. QoS priority queues with weighted round robin
- [ ] 7. Rate limiting with a token bucket
- [ ] 8. VLANs and a trunk port
- [ ] 9. Port mirroring
- [ ] 10. Live reconfiguration
- [ ] 11. swctl admin tool and shared memory stats (done: show mac, show ports, clear mac, stats, stats --watch; todo: shutdown, up, vlan, mirror)
- [ ] 12. Signals with sigwait (done: SIGINT clean shutdown, SIGUSR1 dump; todo: SIGHUP config reload)
- [ ] 13. Logger thread

Tier 3 (Bonus):

- [ ] Two switches linked by pipes
- [ ] Supervisor
- [ ] Benchmark
