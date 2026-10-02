# desktop/csd/Makefile - liblp-csd.so, see lp-csd.c.
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -fPIC

liblp-csd.so: lp-csd.c
	$(CC) $(CFLAGS) -shared -o $@ lp-csd.c -ldl -lpthread -lwayland-client

clean:
	rm -f liblp-csd.so
