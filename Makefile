CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS = -lncurses

PROG = space-travel

$(PROG): space-travel.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(PROG)
	install -m 755 $(PROG) $(PREFIX)/bin/$(PROG)

clean:
	rm -f $(PROG)

.PHONY: clean install
