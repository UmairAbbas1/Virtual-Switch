CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread

all: switch host swctl test

switch: switch.c frame.c mac_table.c shm_stats.c frame.h mac_table.h shm_stats.h
	$(CC) $(CFLAGS) -o switch switch.c frame.c mac_table.c shm_stats.c

host: host.c frame.c frame.h
	$(CC) $(CFLAGS) -o host host.c frame.c

swctl: swctl.c frame.c shm_stats.c frame.h shm_stats.h
	$(CC) $(CFLAGS) -o swctl swctl.c frame.c shm_stats.c

test: test.c frame.c mac_table.c frame.h mac_table.h
	$(CC) $(CFLAGS) -o test test.c frame.c mac_table.c

tsan: switch.c frame.c mac_table.c shm_stats.c
	$(CC) $(CFLAGS) -fsanitize=thread -o switch_tsan switch.c frame.c mac_table.c shm_stats.c

clean:
	rm -f switch host swctl test switch_tsan test_tsan
