CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread

all: test

test: test.c frame.c mac_table.c frame.h mac_table.h
	$(CC) $(CFLAGS) -o test test.c frame.c mac_table.c

tsan: test.c frame.c mac_table.c
	$(CC) $(CFLAGS) -fsanitize=thread -o test_tsan test.c frame.c mac_table.c

clean:
	rm -f test test_tsan
