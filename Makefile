CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread

all: switch host test

switch: switch.c frame.c mac_table.c frame.h mac_table.h
	$(CC) $(CFLAGS) -o switch switch.c frame.c mac_table.c

host: host.c frame.c frame.h
	$(CC) $(CFLAGS) -o host host.c frame.c

test: test.c frame.c mac_table.c frame.h mac_table.h
	$(CC) $(CFLAGS) -o test test.c frame.c mac_table.c

tsan: switch.c frame.c mac_table.c
	$(CC) $(CFLAGS) -fsanitize=thread -o switch_tsan switch.c frame.c mac_table.c

clean:
	rm -f switch host test switch_tsan test_tsan
