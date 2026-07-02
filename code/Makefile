# Makefile - Telemetry collector for Raspberry Pi (Linux / user space)
#
# Build:   make
# Clean:   make clean
# Debug:   make DEBUG=1            (adds -g -O0, keeps asserts)
# Leaks:   make valgrind           (needs valgrind installed)
#
# Dependencies (Raspberry Pi OS / Debian):
#   sudo apt-get update
#   sudo apt-get install -y build-essential libwebsockets-dev libcjson-dev \
#                           ca-certificates
#
# ca-certificates is required so the TLS (wss://) handshake can validate the
# Bluesky server certificate.

CC      ?= gcc
TARGET   = telemetry
SRCS     = main.c producer.c consumer.c logger.c ring_buffer.c stats.c
OBJS     = $(SRCS:.c=.o)
DEPS     = config.h app.h ring_buffer.h stats.h

# -O2 for throughput on the Pi; the warning set catches the usual foot-guns.
CFLAGS  ?= -O2 -Wall -Wextra -Wshadow -std=c11 -D_GNU_SOURCE -pthread
LDFLAGS ?=
LDLIBS   = -lwebsockets -lcjson -lpthread

ifdef DEBUG
CFLAGS  := -O0 -g -Wall -Wextra -Wshadow -std=c11 -D_GNU_SOURCE -pthread
endif

.PHONY: all clean valgrind run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

# Memory-leak / error check over a short run (stop with Ctrl-C after a while).
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all \
	         --track-origins=yes --error-exitcode=1 ./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
