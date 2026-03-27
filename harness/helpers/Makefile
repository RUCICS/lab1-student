CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE

all: probe

probe: probe.c sideband_common.h
	$(CC) $(CFLAGS) -o $@ probe.c

clean:
	rm -f probe

.PHONY: all clean
