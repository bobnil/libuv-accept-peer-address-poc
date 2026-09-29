#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

typedef struct {
  uv_tcp_t handle;
} client_t;

typedef struct {
  uv_loop_t* loop;
  uv_tcp_t server;
  uint64_t accepted;
  uint64_t errors;
  uint64_t target;
  uint64_t started_ns;
  int quiet;
} server_state_t;

static void usage(const char* argv0) {
  fprintf(stderr,
          "Usage: %s [--host 127.0.0.1|::1] --port PORT --count N "
          "[--backlog N] [--quiet]\n",
          argv0);
}

static uint64_t parse_u64(const char* value, const char* name) {
  char* end;
  unsigned long long n;

  errno = 0;
  n = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    fprintf(stderr, "Invalid %s: %s\n", name, value);
    exit(2);
  }

  return (uint64_t) n;
}

static void close_client_cb(uv_handle_t* handle) {
  client_t* client;

  client = (client_t*) handle->data;
  free(client);
}

static void maybe_close_server(server_state_t* state) {
  if (state->accepted + state->errors >= state->target)
    uv_close((uv_handle_t*) &state->server, NULL);
}

static void connection_cb(uv_stream_t* server, int status) {
  server_state_t* state;
  client_t* client;
  int rc;

  state = (server_state_t*) server->data;
  if (status < 0) {
    state->errors++;
    maybe_close_server(state);
    return;
  }

  client = calloc(1, sizeof(*client));
  if (client == NULL) {
    state->errors++;
    maybe_close_server(state);
    return;
  }

  rc = uv_tcp_init(state->loop, &client->handle);
  if (rc == 0)
    rc = uv_accept(server, (uv_stream_t*) &client->handle);

  if (rc == 0) {
    client->handle.data = client;
    state->accepted++;
    uv_close((uv_handle_t*) &client->handle, close_client_cb);
  } else {
    free(client);
    state->errors++;
  }

  maybe_close_server(state);
}

int main(int argc, char** argv) {
  const char* host;
  uint64_t count;
  int port;
  int backlog;
  int quiet;
  int rc;
  int i;
  struct sockaddr_storage addr;
  server_state_t state;

  host = "127.0.0.1";
  port = 0;
  count = 0;
  backlog = 4096;
  quiet = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (int) parse_u64(argv[++i], "port");
    } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
      count = parse_u64(argv[++i], "count");
    } else if (strcmp(argv[i], "--backlog") == 0 && i + 1 < argc) {
      backlog = (int) parse_u64(argv[++i], "backlog");
    } else if (strcmp(argv[i], "--quiet") == 0) {
      quiet = 1;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (port <= 0 || port > 65535 || count == 0) {
    usage(argv[0]);
    return 2;
  }

  memset(&state, 0, sizeof(state));
  state.loop = uv_default_loop();
  state.target = count;
  state.quiet = quiet;

  rc = uv_tcp_init(state.loop, &state.server);
  if (rc != 0) {
    fprintf(stderr, "uv_tcp_init: %s\n", uv_strerror(rc));
    return 1;
  }

  if (strchr(host, ':') != NULL)
    rc = uv_ip6_addr(host, port, (struct sockaddr_in6*) &addr);
  else
    rc = uv_ip4_addr(host, port, (struct sockaddr_in*) &addr);

  if (rc != 0) {
    fprintf(stderr, "address parse failed: %s\n", uv_strerror(rc));
    return 1;
  }

  rc = uv_tcp_bind(&state.server, (const struct sockaddr*) &addr, 0);
  if (rc != 0) {
    fprintf(stderr, "uv_tcp_bind: %s\n", uv_strerror(rc));
    return 1;
  }

  state.server.data = &state;
  rc = uv_listen((uv_stream_t*) &state.server, backlog, connection_cb);
  if (rc != 0) {
    fprintf(stderr, "uv_listen: %s\n", uv_strerror(rc));
    return 1;
  }

  state.started_ns = uv_hrtime();
  rc = uv_run(state.loop, UV_RUN_DEFAULT);
  if (rc != 0) {
    fprintf(stderr, "uv_run returned active handles: %d\n", rc);
    return 1;
  }

  {
    uint64_t elapsed_ns;
    double elapsed_s;
    double rate;

    elapsed_ns = uv_hrtime() - state.started_ns;
    elapsed_s = (double) elapsed_ns / 1000000000.0;
    rate = elapsed_s > 0.0 ? (double) state.accepted / elapsed_s : 0.0;

    if (!quiet) {
      printf("accepted=%" PRIu64 " errors=%" PRIu64
             " seconds=%.6f accepts_per_second=%.2f\n",
             state.accepted,
             state.errors,
             elapsed_s,
             rate);
    } else {
      printf("%" PRIu64 ",%" PRIu64 ",%.6f,%.2f\n",
             state.accepted,
             state.errors,
             elapsed_s,
             rate);
    }
  }

  return state.errors == 0 && state.accepted == state.target ? 0 : 1;
}
