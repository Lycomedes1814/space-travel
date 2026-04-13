CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS = -lncursesw

PROG = space-travel
PREFIX ?= /usr/local

$(PROG): space-travel.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(PROG)
	install -m 755 $(PROG) $(PREFIX)/bin/$(PROG)

clean:
	rm -f $(PROG) *.o

.PHONY: clean install
