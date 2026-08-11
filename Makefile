CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2
LDFLAGS =

TARGET = DirectoryScan
OBJS = main.o scanner.o display.o watcher.o fileutil.o config.o usnwatcher.o log.o

# Install paths
ifeq ($(OS),Windows_NT)
    PREFIX ?= $(USERPROFILE)
    BINDIR = $(PREFIX)/bin
    EXE_EXT = .exe
else
    PREFIX ?= /usr/local
    BINDIR = $(PREFIX)/bin
    EXE_EXT =
endif

FULL_TARGET = $(TARGET)$(EXE_EXT)

.PHONY: all clean install

all: $(FULL_TARGET)

$(FULL_TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(FULL_TARGET)
	mkdir -p "$(BINDIR)"
	cp "$(FULL_TARGET)" "$(BINDIR)/"

clean:
	rm -f $(OBJS) $(FULL_TARGET)
