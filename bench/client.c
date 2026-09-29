#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int fd;
  int active;
} slot_t;

static void usage(const char* argv0) {
  fprintf(stderr,
          "Usage: %s [--host 127.0.0.1|::1] --port PORT --count N "
          "[--parallel N] [--quiet]\n",
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

static uint64_t now_ns(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    perror("clock_gettime");
    exit(1);
  }

  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static int set_nonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int start_connect(const struct addrinfo* ai) {
  int fd;
  int rc;

  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == -1)
    return -1;

  if (set_nonblock(fd) != 0) {
    close(fd);
    return -1;
  }

  rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
  if (rc == 0 || errno == EINPROGRESS)
    return fd;

  close(fd);
  return -1;
}

int main(int argc, char** argv) {
  const char* host;
  const char* port;
  uint64_t count;
  uint64_t started;
  uint64_t completed;
  uint64_t failed;
  uint64_t start_ns;
  uint64_t elapsed_ns;
  size_t parallel;
  size_t active;
  int quiet;
  int i;
  struct addrinfo hints;
  struct addrinfo* ai;
  slot_t* slots;
  struct pollfd* pfds;

  host = "127.0.0.1";
  port = NULL;
  count = 0;
  parallel = 256;
  quiet = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = argv[++i];
    } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
      count = parse_u64(argv[++i], "count");
    } else if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc) {
      parallel = (size_t) parse_u64(argv[++i], "parallel");
    } else if (strcmp(argv[i], "--quiet") == 0) {
      quiet = 1;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (port == NULL || count == 0 || parallel == 0) {
    usage(argv[0]);
    return 2;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  i = getaddrinfo(host, port, &hints, &ai);
  if (i != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(i));
    return 1;
  }

  slots = calloc(parallel, sizeof(*slots));
  pfds = calloc(parallel, sizeof(*pfds));
  if (slots == NULL || pfds == NULL) {
    perror("calloc");
    return 1;
  }

  for (i = 0; (size_t) i < parallel; i++)
    slots[i].fd = -1;

  started = 0;
  completed = 0;
  failed = 0;
  active = 0;
  start_ns = now_ns();

  while (completed + failed < count) {
    while (started < count && active < parallel) {
      size_t idx;
      int fd;

      for (idx = 0; idx < parallel; idx++)
        if (!slots[idx].active)
          break;

      fd = start_connect(ai);
      started++;

      if (fd == -1) {
        failed++;
        continue;
      }

      slots[idx].fd = fd;
      slots[idx].active = 1;
      active++;
    }

    {
      size_t n;
      size_t idx;
      int rc;

      n = 0;
      for (idx = 0; idx < parallel; idx++) {
        if (!slots[idx].active)
          continue;

        pfds[n].fd = slots[idx].fd;
        pfds[n].events = POLLOUT;
        pfds[n].revents = 0;
        n++;
      }

      if (n == 0)
        continue;

      rc = poll(pfds, n, -1);
      if (rc < 0) {
        if (errno == EINTR)
          continue;
        perror("poll");
        return 1;
      }

      n = 0;
      for (idx = 0; idx < parallel; idx++) {
        int so_error;
        socklen_t so_error_len;

        if (!slots[idx].active)
          continue;

        if (pfds[n].revents != 0) {
          so_error = 0;
          so_error_len = sizeof(so_error);
          if (getsockopt(slots[idx].fd,
                         SOL_SOCKET,
                         SO_ERROR,
                         &so_error,
                         &so_error_len) == 0 && so_error == 0) {
            completed++;
          } else {
            failed++;
          }

          close(slots[idx].fd);
          slots[idx].fd = -1;
          slots[idx].active = 0;
          active--;
        }

        n++;
      }
    }
  }

  elapsed_ns = now_ns() - start_ns;
  {
    double elapsed_s;
    double rate;

    elapsed_s = (double) elapsed_ns / 1000000000.0;
    rate = elapsed_s > 0.0 ? (double) completed / elapsed_s : 0.0;

    if (!quiet) {
      printf("completed=%" PRIu64 " failed=%" PRIu64
             " seconds=%.6f connects_per_second=%.2f\n",
             completed,
             failed,
             elapsed_s,
             rate);
    } else {
      printf("%" PRIu64 ",%" PRIu64 ",%.6f,%.2f\n",
             completed,
             failed,
             elapsed_s,
             rate);
    }
  }

  freeaddrinfo(ai);
  free(slots);
  free(pfds);

  return failed == 0 && completed == count ? 0 : 1;
}
