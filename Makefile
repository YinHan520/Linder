CC      ?= gcc
PKGCONFIG ?= pkg-config
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

# GTK3 依赖（view.c 自绘窗口需要）
GTK_CFLAGS  := $(shell $(PKGCONFIG) --cflags gtk+-3.0)
GTK_LDFLAGS := $(shell $(PKGCONFIG) --libs gtk+-3.0) $(shell $(PKGCONFIG) --libs x11) -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC := src/main.c src/dsstore.c src/dsstore_parse.c src/dsstore_write.c src/apply.c src/view.c src/settings.c
OBJ := $(SRC:.c=.o)
BIN := linder

# Offscreen render tool (output PNG, no X display needed)
RENDER_SRC := src/render.c src/dsstore.c src/dsstore_parse.c
RENDER_BIN := Linder-render

all: $(BIN) $(RENDER_BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(GTK_LDFLAGS) $(LDFLAGS)

$(RENDER_BIN): $(RENDER_SRC) src/dsstore.h src/dsstore_parse.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $(RENDER_SRC) $(GTK_LDFLAGS) $(LDFLAGS)

src/%.o: src/%.c src/dsstore.h src/dsstore_parse.h src/dsstore_write.h src/apply.h src/view.h src/settings.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c -o $@ $<

install: $(BIN)
	install -m 0755 $(BIN) $(BINDIR)/$(BIN)
	ln -sf $(BIN) $(BINDIR)/Linder

uninstall:
	rm -f $(BINDIR)/$(BIN) $(BINDIR)/Linder

clean:
	rm -f $(OBJ) $(BIN) $(RENDER_BIN)

.PHONY: all install uninstall clean
