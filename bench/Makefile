CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -Wall -Wextra -std=c11
CPPFLAGS ?= -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112
LDFLAGS ?=
LDLIBS ?= -pthread -ldl -lrt

ROOT := ..

UPSTREAM_DIR := $(ROOT)/libuv-upstream
PATCHED_DIR := $(ROOT)/libuv-patched
UPSTREAM_BUILD := $(UPSTREAM_DIR)/build
PATCHED_BUILD := $(PATCHED_DIR)/build

SERVER_SRC := server.c
CLIENT_SRC := client.c

.PHONY: all clean

all: server-upstream server-patched client

server-upstream: $(SERVER_SRC) $(UPSTREAM_BUILD)/libuv.a
	$(CC) $(CPPFLAGS) -I$(UPSTREAM_DIR)/include $(CFLAGS) $< \
		$(UPSTREAM_BUILD)/libuv.a $(LDFLAGS) $(LDLIBS) -o $@

server-patched: $(SERVER_SRC) $(PATCHED_BUILD)/libuv.a
	$(CC) $(CPPFLAGS) -I$(PATCHED_DIR)/include $(CFLAGS) $< \
		$(PATCHED_BUILD)/libuv.a $(LDFLAGS) $(LDLIBS) -o $@

client: $(CLIENT_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f server-upstream server-patched client
