CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

BIN ?= joypadmouse
PREFIX ?= $(HOME)/.local

SRC := src/joypadmouse.c

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -Dm755 scripts/sunshine-joypadmouse-start "$(DESTDIR)$(PREFIX)/bin/sunshine-joypadmouse-start"
	install -Dm755 scripts/sunshine-joypadmouse-stop "$(DESTDIR)$(PREFIX)/bin/sunshine-joypadmouse-stop"
	install -Dm755 scripts/joypadmouse-kill-top "$(DESTDIR)$(PREFIX)/bin/joypadmouse-kill-top"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	rm -f "$(DESTDIR)$(PREFIX)/bin/sunshine-joypadmouse-start"
	rm -f "$(DESTDIR)$(PREFIX)/bin/sunshine-joypadmouse-stop"
	rm -f "$(DESTDIR)$(PREFIX)/bin/joypadmouse-kill-top"

