# Lab Task: Virtual Switch - Protocol Header & Concurrent MAC Table

## Files

- frame.h / frame.c : 17-byte packed frame header, serialize and deserialize using htons / ntohs
- mac_table.h / mac_table.c : MAC table (MAC -> port) with 16 buckets and one mutex per bucket (striped locking)
- test.c : test program for the header and the MAC table (single thread and 4 threads)
- Makefile

## Build

    make

## Run

    ./test

## ThreadSanitizer

    make tsan
    setarch -R ./test_tsan

## Valgrind

    valgrind --leak-check=full ./test

## Clean

    make clean

## Header format

| Field    | Size    | Offset |
|----------|---------|--------|
| length   | 2 bytes | 0      |
| src      | 6 bytes | 2      |
| dst      | 6 bytes | 8      |
| vlan     | 2 bytes | 14     |
| priority | 1 byte  | 16     |

length and vlan are stored in network byte order.

## MAC table

- 16 buckets, each bucket is a linked list with its own pthread_mutex_t
- mac_table_lookup() returns the port, or -1 if the MAC is not found
- mac_table_insert() adds a new MAC, or updates the port if the MAC moved
- When a MAC moves it prints: MAC moved: <mac> port X -> port Y

## Expected output

    header size = 17
    bytes: 05 DC AA BB CC 00 00 01 FF FF FF FF FF FF 00 0A 01
    length=1500 vlan=10 priority=1 src=AA:BB:CC:00:00:01 dst=FF:FF:FF:FF:FF:FF
    lookup a -> port 1
    MAC moved: AA:BB:CC:00:00:01 port 1 -> port 3
    lookup a -> port 3
    lookup unknown -> port -1
    threads done
