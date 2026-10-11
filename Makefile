# terminal — blazing fast GPU-accelerated terminal emulator
CC      ?= cc
PKGS     = glfw3 freetype2 luajit

CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -Ivendor $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) \
           -framework OpenGL -framework Cocoa -framework IOKit \
           -framework CoreVideo -framework UniformTypeIdentifiers -framework AVFoundation \
           -framework CoreServices -lpthread

SRC = src/main.c src/term.c src/render.c src/pty.c src/plugin.c src/viewer.c src/editor.c src/player.c src/links.c
OBJ = $(SRC:.c=.o) src/mac_open.o src/mac_defaults.o src/mac_audio.o
BIN = terminal

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c src/terminal.h src/viewer.h src/editor.h src/player.h src/links.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.m
	$(CC) $(CFLAGS) -fobjc-arc -c -o $@ $<

run: $(BIN)
	./$(BIN)

debug: CFLAGS += -g -O0 -fsanitize=address
debug: LDFLAGS += -fsanitize=address
debug: clean $(BIN)

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/$(BIN)

# macOS app bundle — enables Finder "Open With" / default file handler
app: $(BIN)
	rm -rf terminal.app
	mkdir -p terminal.app/Contents/MacOS terminal.app/Contents/Resources
	cp $(BIN) terminal.app/Contents/MacOS/terminal
	cp assets/Info.plist terminal.app/Contents/Info.plist
	cp assets/terminal.icns terminal.app/Contents/Resources/terminal.icns
	@echo "built terminal.app — run scripts/set-default-handlers.sh to register"

register: app
	sh scripts/set-default-handlers.sh

# self-contained app + dist/terminal.dmg + dist/terminal.pkg
package:
	sh scripts/package.sh

.PHONY: all run debug clean install app register package
