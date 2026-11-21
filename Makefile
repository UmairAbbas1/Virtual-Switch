CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread

SWITCH_SRC = switch.c frame.c mac_table.c shm_stats.c logger.c config.c
HEADERS = frame.h mac_table.h shm_stats.h logger.h config.h

all: switch host swctl test

switch: $(SWITCH_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o switch $(SWITCH_SRC)

host: host.c frame.c frame.h
	$(CC) $(CFLAGS) -o host host.c frame.c

swctl: swctl.c frame.c shm_stats.c frame.h shm_stats.h
	$(CC) $(CFLAGS) -o swctl swctl.c frame.c shm_stats.c

test: test.c frame.c mac_table.c logger.c frame.h mac_table.h logger.h
	$(CC) $(CFLAGS) -o test test.c frame.c mac_table.c logger.c

tsan: $(SWITCH_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -fsanitize=thread -o switch_tsan $(SWITCH_SRC)

clean:
	rm -f switch host swctl test switch_tsan test_tsan
