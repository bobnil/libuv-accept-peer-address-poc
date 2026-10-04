# Historical context: TCP peer-address lifetime in Node.js and libuv

This document records the historical context behind the loss of TCP peer-address information that can occur when a
connection is accepted, then reset or destroyed before `uv_tcp_getpeername()` is queried.

It is intended as supporting material for the corresponding libuv issue and proof of concept. The goal is not to assign
blame to any particular change. In fact, the history suggests that the current behavior emerged from several
individually reasonable API and performance decisions.

The central distinction is:

```text
peer sockaddr returned by accept()
    = information captured at the moment a connection is accepted

getpeername(accepted_fd) later
    = a query against the socket's current state
```

Those normally describe the same peer. They are not necessarily equivalent after a socket-state transition such as an
RST. If the later `getpeername()` fails, an address that was available at accept time may no longer be recoverable.

## Executive summary

The relevant history can be summarized as follows:

1. The original 4.2BSD sockets interface allowed `accept()` to return the connecting peer's address with the accepted
   socket. Berkeley's contemporary primer demonstrates this server pattern. `getpeername()` was a separate query on a
   connected socket, not a required second stage of `accept()`.

2. BSD-derived code exposes the difference in lifetime: a later `getpeername()` can return `ENOTCONN` after a socket
   loses its connected state, even when the accepted connection had a known peer.

3. Linux's implementation evolved. Linux 0.98 stored peer information on the pending child but used a state-sensitive
   `getname()` to fill the `accept()` address. In 2.3.15 Linux explicitly retained an established-then-RST child for
   `accept()`; in 2.3.43 it made accept-time address retrieval (`peer=2`) distinct from ordinary `getpeername()`
   (`peer=1`). The combined behavior allows `accept()` to return the stored address for a child in `TCP_CLOSE` while a
   later `getpeername()` returns `ENOTCONN`.

4. Before libuv, Node's legacy networking implementation stored the address and port returned by its `accept()` binding
   on the new socket before emitting `connection`.

5. The first public Node release with libuv, v0.5.0 (July 2011), already had a different abstraction. Unix libuv
   received a peer sockaddr in its internal accept path but retained only the accepted fd pending `uv_accept()`. The
   Windows backend likewise transferred the accepted socket through `uv_accept()` without retaining accept-time address
   data as peer identity on the client handle.

6. In September 2011, libuv added `uv_tcp_getpeername()` as a separate live socket query, which Node used for
   `remoteAddress` and `remotePort`. Node initially queried before emitting the connection.

7. In October 2011, Node made that query lazy, reporting about a 1% improvement in the connection-heavy `http_simple`
   benchmark, which did not use peer identity. This widened the interval before the first query.

8. In May 2012, libuv stopped asking Unix `accept()` / `accept4()` to fill the otherwise unused sockaddr, reporting a
   saving of a few nanoseconds. The abstraction had already discarded this data; that change did not create the earlier
   loss.

9. Node issue #7566 identified the accept/query race in 2014 and suggested retaining the address with the accepted
   connection. A 2015 change preserved an already cached peer name after close, but could not help when the first lookup
   occurred after destruction. The symptom remained reproducible in 2018, and the first-access limitation was documented
   in 2023.

10. In the reviewed current Unix libuv code, the accept path uses `NULL, NULL`, retains the accepted fd, and obtains the
    peer through a later `getpeername()` when requested.

The resulting behavior follows from three separate choices:

```text
accept-time peer address is not retained
+ peer lookup happens later (eventually lazily)
+ later lookup depends on current socket state
```

The original BSD API did not require this sequence. Linux's 2.3.43 implementation makes the difference between the two
address requests explicit. The sources do not establish the original libuv authors' reason for excluding the accept-time
address.

---

## 1. Socket API baseline: BSD and Linux

The sockets interface offered a way to obtain peer identity at acceptance from its introduction. The distinction between
that result and a later socket query matters because a connection can change state before application code asks for its
peer.

### 1.1 The original BSD interface

Berkeley's 1983 _4.2bsd Interprocess Communication Primer_ gives this server sequence:

```c
fromlen = sizeof (from);
snew = accept(s, &from, &fromlen);
```

It explains that a server interested in who connected supplies a buffer for the client's socket name; otherwise the
address argument may be omitted. The historical `accept(2)` manual likewise describes the address as a result parameter
filled with the connecting entity's address. This capability belongs to the original interface, not to a later
extension.

`getpeername()` also originated in 4.2BSD, but it queries the peer of an existing connected socket. Its documented
failures include `ENOTCONN` when the socket is not connected. The interface thus does not require
`accept(fd, NULL, NULL)` followed by `getpeername()` to learn the newly accepted peer. This does not claim that no
historical program ever used that sequence.

Historical BSD-derived FreeBSD kernel code makes the state dependency concrete: its `getpeername()` path checks
`SS_ISCONNECTED | SS_ISCONFIRMING` and returns `ENOTCONN` if neither bit is set, whereas its accept path can provide an
address while accepting the connection. A January 2011 FreeBSD networking discussion reports `getpeername()` returning
`ENOTCONN` after an established TCP connection received an RST. The example illustrates the lifetime difference, without
making a universal claim about every BSD implementation or every reset.

### 1.2 Early Linux: stored peer, state-sensitive address lookup

Linux 0.98 created the TCP child before userspace called `accept()`, storing the peer IP and port on that child. Its
generic accept path then asked the protocol for the peer name to fill the caller's address buffer. The TCP peer-name
operation required `TCP_ESTABLISHED`, and the generic accept path ignored a failure from that lookup. Consequently, the
child and its stored peer could exist even when `accept()` did not successfully return the address. The complete later
Linux behavior must not be projected back onto 0.98.

The 1.1 series explicitly discussed BSD accept/select expectations. Linux 1.1.13 noted that BSD `accept()` would not
fail after signalling readiness through `select()`, whereas Linux could. Changes in 1.1.50 aligned readiness and
acceptability checks; a later changelog called this “BSD accept semantics.” Linux 1.1.63 refined the eligible states to
`TCP_ESTABLISHED` or states from `TCP_FIN_WAIT1` onward, excluding SYN states. Linux 1.3.0 says the TCP layer now gives
BSD semantics. These changes did not yet prove that a child reset before accept would be returned with its peer address:
an error-bearing child could still be rejected in the higher-level accept path.

### 1.3 Linux 2.3.15 retains an established-then-RST child

The Linux 2.3.15 patch explicitly describes a connection established and then killed by RST before userspace accepted
it. Its comment says the pending request must remain to avoid blocking in `accept()`, which will collect the dead child.
The same patch removes the earlier higher-level rejection and explicitly allows `TCP_CLOSE` among states of a child
returned by protocol accept.

This establishes the lifetime of the reset child, but does not by itself establish that `accept()` can return its
address: address retrieval still used the ordinary peer-name mode.

### 1.4 Linux 2.3.43 separates the two peer-name modes

The patch from Linux 2.3.42 to 2.3.43 changes the generic `accept()` address lookup from `getname(..., 1)` to
`getname(..., 2)`. Normal `getpeername()` continues to use `peer=1`. The IPv4 `inet_getname()` checks for `TCP_CLOSE` or
`TCP_SYN_SENT` only when `peer == 1`; with `peer=2`, it can return the stored destination address and port even when the
accepted child is in `TCP_CLOSE`. The patch makes the corresponding distinction in IPv6.

Together, the 2.3.15 and 2.3.43 changes support this specific case:

```text
established child → RST before userspace accept → TCP_CLOSE
          → pending child retained → accept(..., &peer, ...) returns stored peer
          → later getpeername(accepted_fd, ...) returns ENOTCONN
```

The mode distinction is explicit kernel behavior, not just an inference from a reproducer. It also shows why merely
storing a live fd and querying it later can lose information that was available to `accept()`. The reviewed sources
establish this historical Linux case; they do not assert that every failure to query a peer on every kernel follows the
same path.

**Sources:** Berkeley's 1983 primer; historical `accept(2)` and `getpeername(2)` manuals; BSD-derived `uipc_syscalls.c`;
the January 2011 FreeBSD networking report; and the Linux source snapshots and patches detailed under References.

---

## 2. Before libuv: Node retained peer identity from accept

Node's legacy networking backend provides a useful baseline because it shows a different information lifetime.

In Node v0.5.9's `lib/net_legacy.js`, the server calls its `accept` binding and receives an object containing both the
accepted fd and peer information:

```js
var peerInfo = accept(self.fd);
```

The new `Socket` is constructed from `peerInfo.fd`, and before the `connection` event is emitted Node assigns:

```js
s.remoteAddress = peerInfo.address;
s.remotePort = peerInfo.port;
```

Only after that does it emit the socket to user code.

Conceptually:

```text
accept
  ├─ fd
  └─ peer address / port
          ↓
        Socket
          ↓
  connection callback
```

This matters because it demonstrates that retaining accept-time peer identity is not a novel model being proposed
retrospectively. Node's legacy backend already had that property.

**Source**

- Node v0.5.9, `lib/net_legacy.js`, server accept path:
  https://github.com/nodejs/node-v0.x-archive/blob/3bd9b08fb125b606f97a4079b147accfdeebb07d/lib/net_legacy.js#L941-L975

---

## 3. July 2011: the first public libuv backend changes the abstraction

Node v0.5.0 was released on July 5, 2011. Its changelog describes a:

> New non-default libuv backend to support IOCP on Windows.

At this point libuv was still opt-in through `--use-uv`.

The code shipped in this release already contains the core abstraction that is still recognizable today: the
operating-system accept completes internally, libuv remembers an accepted connection, the connection callback runs, and
the application subsequently calls `uv_accept()` to initialize a client handle.

### 3.1 Contemporary API design document

The v0.5.0 libuv tree contains `desired-api.md`, a contemporary design document. Its TCP section says that the accept
callback is triggered by `uv_listen()`, then describes `uv_accept(server, client)` as something that should be called
after that callback. It also describes a server-side `uv_tcp_t` as one initialized by `uv_accept()`.

That is important because the separation was part of the intended API model, not merely an incidental detail of one Unix
implementation.

The high-level model was:

```text
OS accept completes
        ↓
libuv holds accepted connection
        ↓
connection callback
        ↓
uv_accept(server, client)
        ↓
client handle initialized
```

**Sources**

- Node v0.5.0 changelog:
  https://github.com/nodejs/node/blob/main/doc/changelogs/CHANGELOG_ARCHIVE.md#20110705-version-050-unstable
- Node v0.5.0 bundled libuv, `desired-api.md`:
  https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/desired-api.md

### 3.2 Unix: the sockaddr is present, but only the fd is retained

The Unix implementation shipped in v0.5.0 is especially revealing.

`uv__server_io()` allocates:

```c
struct sockaddr_storage addr;
socklen_t addrlen = sizeof(struct sockaddr_storage);
```

and calls:

```c
fd = accept(tcp->fd, (struct sockaddr*)&addr, &addrlen);
```

On success it stores:

```c
tcp->accepted_fd = fd;
```

and invokes the connection callback.

If user code has not yet called `uv_accept()`, the accepted fd remains parked in the server object. Later, `uv_accept()`
initializes the client handle from `accepted_fd`.

The peer sockaddr is not copied to persistent state and is not passed to the client handle.

Thus, by the first public Node release containing libuv, Unix libuv already had:

```text
accept()
  ├─ fd       ───────────────→ retained as accepted_fd
  └─ sockaddr ───────────────→ not retained
```

This is earlier than the introduction of `uv_tcp_getpeername()`.

**Source**

- Node v0.5.0 bundled libuv, `uv-unix.c`:
  https://github.com/nodejs/node-v0.x-archive/blob/ae7ed8482ea7e53c59acbdf3cf0e0a0ae9d792cd/deps/uv/uv-unix.c#L364-L429

### 3.3 Why this is an information-lifetime change

The legacy Node backend and early libuv backend did not merely use different APIs; they gave peer identity different
lifetimes.

Legacy Node:

```text
accept-time peer identity
        ↓
stored on Socket
        ↓
survives later socket state changes
```

Early libuv:

```text
accept-time peer identity
        ↓
temporary local sockaddr
        ↓
discarded
```

At this stage, `uv_tcp_getpeername()` did not yet exist. Therefore it would be incorrect to say that a later
`getpeername()` call _replaced_ propagation of the accept-time sockaddr in September 2011. The accept-time sockaddr was
already not being propagated in July.

---

## 4. The Windows side: the same connection/handle separation

The original libuv work was explicitly motivated by providing a common asynchronous I/O abstraction across Unix and
Windows.

A contemporary document in the v0.5.0 tree, Ryan Dahl's "Asynchronous I/O in Windows for Unix Programmers", maps Unix
`accept(2)` to Windows `AcceptEx()`.

The Windows implementation is relevant because `AcceptEx()` itself can return address information as part of the accept
operation.

### 4.1 The original Windows server state

The v0.5.0 Windows `uv_tcp_t` server fields contain:

```c
SOCKET accept_socket;
struct uv_req_s accept_req;
char accept_buffer[sizeof(struct sockaddr_storage) * 2 + 32];
```

`uv_queue_accept()` invokes `AcceptEx()` with the buffer and reserves space for two `sockaddr_storage` values.

The same source also dynamically retrieves the Winsock extension function pointer for `GetAcceptExSockaddrs()`.

However, `uv_accept()` transfers the accepted socket into the client handle:

```c
uv_tcp_set_socket(tcpClient, tcpServer->accept_socket)
```

then resets `accept_socket` and queues another accept.

The accept-time address buffer is not propagated as persistent peer identity on the client handle.

### 4.2 What this does and does not prove

This directly shows that the original libuv design had a cross-platform "accepted connection first, client handle later"
abstraction on both Unix and Windows.

It also makes it difficult to explain the behavior as simple unawareness that accept mechanisms can provide peer-address
information: the Windows code explicitly allocates the AcceptEx address buffer and loads `GetAcceptExSockaddrs()`.

What the historical record found here does **not** establish is _why_ peer metadata was excluded from the state
propagated through `uv_accept()`. No contemporary comment has been found saying, for example, that the address was
deliberately omitted for ABI size, performance, API simplicity, or any other specific reason.

The safest historical conclusion is therefore:

> The original libuv abstraction propagated an accepted connection into a client handle, but did not make accept-time
> endpoint identity persistent state on that handle.

The broader cross-platform design goal is documented. The specific reason for excluding peer metadata is not.

**Sources**

- Node v0.5.0 bundled libuv, `uv-win.h`:
  https://github.com/nodejs/node-v0.x-archive/blob/ae7ed8482ea7e53c59acbdf3cf0e0a0ae9d792cd/deps/uv/uv-win.h#L64-L79
- Node v0.5.0 bundled libuv, `uv-win.c`, AcceptEx setup:
  https://github.com/nodejs/node-v0.x-archive/blob/ae7ed8482ea7e53c59acbdf3cf0e0a0ae9d792cd/deps/uv/uv-win.c#L823-L871
- Node v0.5.0 bundled libuv, `uv-win.c`, `uv_accept()`:
  https://github.com/nodejs/node-v0.x-archive/blob/ae7ed8482ea7e53c59acbdf3cf0e0a0ae9d792cd/deps/uv/uv-win.c#L940-L964
- Node v0.5.0 bundled libuv, Winsock extension loading:
  https://github.com/nodejs/node-v0.x-archive/blob/ae7ed8482ea7e53c59acbdf3cf0e0a0ae9d792cd/deps/uv/uv-win.c#L409-L444
- Ryan Dahl, `iocp-links.html`, Unix `accept(2)` / Windows `AcceptEx()` mapping:
  https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/iocp-links.html

---

## 5. September 2011: `uv_tcp_getpeername()` is introduced

The exact libuv commit introducing `uv_tcp_getpeername()` is:

- `12b01e95f9afb56f602ca17f44d3b7e22e37c656`
- `Specialize uv_xxx_getsockname, add uv_tcp_getpeername`
- author: Bert Belder (`piscisaureus`)

The commit adds the public `uv_tcp_getpeername()` API.

On Unix, the implementation is a live query against the socket using `getpeername()`.

The same commit adds `ENOTCONN` to libuv's Unix error translation.

That does not prove that reset-after-accept behavior was considered at the time. `ENOTCONN` has many legitimate uses. It
is nevertheless worth noting that the peer-query API was state-dependent from its introduction.

A second useful detail is what the commit does **not** change. Its Unix changes touch `core.c`, `error.c`, `tcp.c`, and
`udp.c`; it does not modify the stream accept path. Therefore the Unix behavior described above -- receiving a sockaddr
during accept but retaining only the fd -- predates `uv_tcp_getpeername()`.

**Commit**

- libuv `12b01e95f9afb56f602ca17f44d3b7e22e37c656`:
  https://github.com/libuv/libuv/commit/12b01e95f9afb56f602ca17f44d3b7e22e37c656

---

## 6. Node adopts `uv_tcp_getpeername()` for `remoteAddress`

The next important Node commit is:

- `e20d0c1`
- `net-uv: correctly set socket.remoteAddress and -port`
- closes Node issue #1345
- author: Bert Belder

Before this fix, the libuv backend incorrectly used local socket information for the remote endpoint.

The change adds a `getpeername()` binding to `TCPWrap` that calls `uv_tcp_getpeername()` and uses it from the connection
path.

Node v0.5.6, released September 8, 2011, lists both:

- `#1345 Correctly set socket.remoteAddress with libuv backend`
- `#1503 Make libuv backend default on unix, override with node --use-legacy`

and upgrades its bundled libuv to commit `bd6066cb349a9b3a1b0d87b146ddaee06db31d10`.

This is the point where the libuv backend's peer identity became operationally important for normal Unix Node users.

**Sources**

- Node commit `e20d0c1`: https://github.com/nodejs/node-v0.x-archive/commit/e20d0c1
- Node v0.5.6 changelog:
  https://github.com/nodejs/node/blob/main/doc/changelogs/CHANGELOG_ARCHIVE.md#20110908-version-056-unstable

---

## 7. Initially Node queried peer identity immediately

In the early libuv backend, Node did not yet defer peer lookup until a property was accessed.

Node v0.5.9's `lib/net_uv.js` effectively did:

```text
accept inside libuv
        ↓
clientHandle.getpeername()
        ↓
store remoteAddress / remotePort
        ↓
emit connection
```

If peer lookup failed, Node closed the handle and entered an error path before emitting a normal `connection` event.

This means that the fundamental semantic race already existed -- the accept-time sockaddr had been discarded and Node
was re-querying the live socket -- but the time window was relatively short and the externally visible failure mode was
different.

**Source**

- Node v0.5.9, `lib/net_uv.js`: https://github.com/nodejs/node-v0.x-archive/blob/v0.5.9/lib/net_uv.js#L706-L754

---

## 8. October 2011: the peer lookup is made lazy for performance

A major change occurred in Node commit:

- `1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0`
- `net: remove unconditional getpeername() call`
- author: Ben Noordhuis
- committed by Ryan Dahl

The commit message says:

> Speeds up http_simple benchmark by about 1.0%

The diff removes the unconditional peer lookup from the connection path and introduces `_getpeername()` plus lazy
`remoteAddress` / `remotePort` getters. The peer information is cached only when one of those properties is first read.

The effective lifetime becomes:

```text
accept
    ↓
connection callback
    ↓
arbitrary JS / event-loop activity
    ↓
socket may reset or be destroyed
    ↓
first remoteAddress / remotePort access
    ↓
getpeername()
```

This significantly increases the time in which socket state can change before the first peer query.

### 8.1 What the `http_simple` benchmark actually exercised

The benchmark source at that commit is useful context.

`benchmark/http_simple.js` handles HTTP requests by inspecting the URL and generating string or buffer response bodies.
It does not inspect the client's remote address or port.

The driver `benchmark/http_simple_bench.sh` runs:

```sh
ab -g $data_fn -c 100 -t 10 ...
```

for string and buffer responses of 1 KiB and 100 KiB.

It does not pass ApacheBench's `-k` option. ApacheBench documents `-k` as the option that enables HTTP KeepAlive and
says the default is no KeepAlive.

Therefore this benchmark generated a connection-heavy workload while never using the peer information whose lookup was
being performed unconditionally.

That makes the 2011 optimization understandable: a separate `getpeername()` syscall was being paid on every connection
even when the application did not need the result.

It is also important when comparing that optimization with accept-time address retention today. Retaining the sockaddr
returned by `accept()` does **not** restore the additional syscall removed by `1bb820a`.

**Sources**

- Node commit `1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0`:
  https://github.com/nodejs/node-v0.x-archive/commit/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0
- `http_simple.js` at that commit:
  https://github.com/nodejs/node-v0.x-archive/blob/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0/benchmark/http_simple.js
- `http_simple_bench.sh` at that commit:
  https://github.com/nodejs/node-v0.x-archive/blob/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0/benchmark/http_simple_bench.sh
- ApacheBench documentation: https://httpd.apache.org/docs/current/en/programs/ab.html

---

## 9. May 2012: stop requesting the unused sockaddr

In May 2012, libuv commit:

- `752ac30ec820bc0ef4dfea698fd7119a8a4aa14c`
- `unix: don't pass sockaddr to accept()`
- author: Ben Noordhuis

changed the Unix accept helper from receiving a sockaddr destination to calling `accept()` / `accept4()` with
`NULL, NULL`.

The commit message says:

> Shaves a few nanoseconds off the accept() syscall.

Context is important here.

Technically, this is correct. If the returned address is not going to be used, avoiding the kernel work required to
populate/copy the sockaddr to userspace can save some time.

Viewed in its historical context, this was a reasonable cleanup/optimization: libuv stopped retrieving information that
its existing abstraction was already discarding.

The diff makes that context concrete. In `src/unix/stream.c` it removes:

```c
struct sockaddr_storage addr;
```

and changes:

```c
fd = uv__accept(stream->fd, (struct sockaddr*)&addr, sizeof addr);
```

to:

```c
fd = uv__accept(stream->fd);
```

while `uv__accept()` itself changes to `accept(sockfd, NULL, NULL)` / `accept4(sockfd, NULL, NULL, ...)`.

The change did **not** introduce the loss of accept-time peer identity. That behavior already existed in the first
public libuv backend. It merely stopped paying the cost of obtaining data that was not being retained.

This distinction separates two different performance questions:

```text
2011 Node optimization:
    accept()
    + separate getpeername()
        ↓
    accept()
    + lazy getpeername only if needed

2012 libuv optimization:
    accept(&sockaddr, &len)
        ↓
    accept(NULL, NULL)
```

A proposal to preserve the address already produced by `accept()` would not reintroduce the separate unconditional
`getpeername()` syscall removed in 2011.

**Commit**

- libuv `752ac30ec820bc0ef4dfea698fd7119a8a4aa14c`:
  https://github.com/libuv/libuv/commit/752ac30ec820bc0ef4dfea698fd7119a8a4aa14c

---

## 10. 2014: Node issue #7566 identifies the same root cause and discusses a fix

Node issue #7566, **"Race condition when getting remoteAddress of connection"**, was opened on May 6, 2014 and labeled
`S-confirmed-bug`, `net`, `v0.10`, and `v0.12`.

The report is remarkably close to the present analysis.

It demonstrates a server receiving a connection for which `remoteAddress` is already unavailable. It then traces the
implementation from Node's `remoteAddress` accessor through `_getpeername`, `TCPWrap::GetPeerName`, and
`uv_tcp_getpeername`.

Most importantly, the report explicitly notes that information can be supplied by `accept()` but is ignored by libuv,
and concludes that the reliable approach would be to retain that address with the connection.

This establishes that the accept/query lifetime problem and the corresponding accept-time-retention approach were both
identified explicitly by 2014.

### 10.1 Ben Noordhuis: current behavior, performance, and ABI constraints

Ben Noordhuis replied that the report's analysis was correct, but described libuv's decision not to request the
accept-time address as intentional and pointed to libuv commit `752ac30`.

He gave two practical reasons. First, having the kernel copy out the `struct sockaddr` has a cost and the result is
often unused. Second, retaining the result would require storing it somewhere; he said that this could not then be done
in `uv_tcp_t` without breaking its ABI.

### 10.2 TJ Fontaine: the problem was known and `uv_accept()` was a constraint

TJ Fontaine distinguished peer and local endpoint information. For both an outbound `net.connect` socket and a socket
created from a server accept, he described `remoteAddress` as information that could be made consistently available
because the peer is implicit in establishing the connection. By contrast, obtaining `localAddress` requires an
additional query.

More importantly, Fontaine said that the absence of an optional `struct sockaddr` from libuv's `uv_accept()` API had
already come up in discussion. He connected that API limitation to release compatibility: it meant the problem could not
be fixed in a stable release, but said that something could be done about it going forward.

He then explicitly acknowledged the current failure mode: applications could fail to receive the address, the project
knew about the problem, and work was underway on a solution intended to improve the situation.

By May 2014, the sources directly establish that:

- the loss of `remoteAddress` was recognized as a real bug;
- the relationship between that failure and libuv's accept/getpeername design was understood;
- retaining information associated with connection establishment was being considered;
- the shape of `uv_accept()` and compatibility with stable libuv releases constrained possible fixes;
- `uv_tcp_t` ABI compatibility was separately identified by Noordhuis as an obstacle to storing the sockaddr directly.

They do **not** establish what exact implementation Fontaine meant by the solution then under development, or that such
a solution subsequently landed.

No libuv change implementing general accept-time peer-address retention has been identified from this discussion.

**Sources**

- Node v0.x archive #7566: https://github.com/nodejs/node-v0.x-archive/issues/7566
- Ben Noordhuis comment, May 6, 2014: https://github.com/nodejs/node-v0.x-archive/issues/7566#issuecomment-42294224
- TJ Fontaine comment, May 6, 2014: https://github.com/nodejs/node-v0.x-archive/issues/7566#issuecomment-42307064

## 11. 2015: preserving an already-cached peer name

In 2015, Node issue #9287 reported that `socket.remoteAddress` became undefined after close even when peer information
had previously been available.

PR #9366 resulted in commit:

- `30666f2`
- `net: use cached peername to resolve remote fields`

The change reordered `_getpeername()` so that an existing `this._peername` cache is checked before testing whether the
native handle still exists.

This solves:

```text
getpeername succeeds
        ↓
_peername is cached
        ↓
socket closes
        ↓
cached remote fields remain readable
```

It does not solve:

```text
socket resets / is destroyed
        ↓
first peer-property access has not happened
        ↓
no _peername cache exists
        ↓
native handle unavailable or getpeername fails
        ↓
peer identity unavailable
```

That distinction is central to understanding why the 2014 race could survive the 2015 fix.

**Sources**

- Node issue #9287: https://github.com/nodejs/node-v0.x-archive/issues/9287
- Node PR #9366: https://github.com/nodejs/node-v0.x-archive/pull/9366
- Node commit `30666f2`: https://github.com/nodejs/node-v0.x-archive/commit/30666f2

---

## 12. 2018: the original symptom is still reproducible

Node issue #23858, opened October 24, 2018, is titled:

> `socket.remoteAddress is sometimes undefined in net server connect handler`

The reporter refers back to #7566 and reproduces the behavior consistently with Nmap.

The important detail is where the properties are accessed:

```js
net.createServer((socket) => {
  // remoteAddress / remotePort / remoteFamily inspected here
});
```

That rules out a narrow interpretation in which the behavior matters only to code that tries to inspect a socket long
after it has closed.

The peer information can already be unavailable when application code first receives the accepted socket.

**Issue**

- Node #23858: https://github.com/nodejs/node/issues/23858

---

## 13. 2023: the first-access-after-destruction limitation is rediscovered

Node issue #48061, opened May 18, 2023, analyzes `_getpeername()` directly.

The issue describes the cache logic:

1. if `_peername` already exists, return it;
2. otherwise, if `_handle` exists, query the handle and create `_peername`;
3. otherwise, return an empty object.

The failure case follows naturally:

```text
socket destroyed before first property access
        ↓
_peername does not exist
_handle no longer exists
        ↓
nothing remains from which to obtain peer identity
```

This is exactly why the 2015 cache change cannot solve the original race.

### 13.1 The resulting change was documentation

PR #48139, merged May 25, 2023, updated Node's documentation so that `remotePort` and `remoteFamily`, like
`remoteAddress`, are documented as possibly `undefined` when the socket has been destroyed.

The PR was reviewed and approved by several Node maintainers, including Ben Noordhuis.

It did not change libuv's accept path or add accept-time peer retention.

**Sources**

- Node #48061: https://github.com/nodejs/node/issues/48061
- Node PR #48139: https://github.com/nodejs/node/pull/48139
- landed commit on main, `5275843`: https://github.com/nodejs/node/commit/5275843

---

## 14. Current libuv: the architecture remains recognizable

Current Unix libuv still follows the same broad model.

### 14.1 Accept does not request the peer sockaddr

`uv__accept()` calls:

```c
uv__accept4(sockfd, NULL, NULL, ...)
```

or:

```c
accept(sockfd, NULL, NULL)
```

**Source**

- current `src/unix/core.c`: https://github.com/libuv/libuv/blob/v1.x/src/unix/core.c#L559-L589

### 14.2 The server retains the accepted fd

`uv__server_io()` stores the result in:

```c
stream->accepted_fd
```

then invokes the connection callback.

`uv_accept()` later opens the client stream from that fd.

**Source**

- current `src/unix/stream.c`: https://github.com/libuv/libuv/blob/v1.x/src/unix/stream.c#L507-L532

### 14.3 Peer identity remains a live query

Current Unix `uv_tcp_getpeername()` delegates to a helper with the operating system's `getpeername()` function:

```c
return uv__getsockpeername(..., getpeername, ...);
```

The public documentation describes the API as:

> Get the address of the peer connected to the handle.

It does not describe the address as a snapshot retained from accept time.

**Sources**

- current `src/unix/tcp.c`: https://github.com/libuv/libuv/blob/v1.x/src/unix/tcp.c#L382-L393
- libuv TCP documentation: https://docs.libuv.org/en/v1.x/tcp.html

---

## 15. The performance history in context

The performance history is easier to understand when the two optimizations are kept separate.

### 15.1 2011: remove a separate syscall

Node's `1bb820a` removed an unconditional **separate `getpeername()` call** for each accepted connection.

That commit reported an approximately 1% `http_simple` improvement.

The benchmark did not consume peer identity, used concurrency 100 for ten seconds per test, and did not enable HTTP
KeepAlive. It was therefore a workload where avoiding unconditional per-connection work could plausibly matter.

### 15.2 2012: stop copying unused output from accept

libuv's `752ac30` addressed a different cost: it stopped asking the existing `accept()` syscall to fill an address
structure that libuv did not use.

Its "few nanoseconds" description should therefore be read in that context.

It was not a choice between "correct peer identity" and "a few nanoseconds". By then the abstraction had already ceased
to retain accept-time peer identity. The commit removed work whose result was unused.

### 15.3 The present POC does not restore the 2011 cost

The current proof of concept compares, conceptually:

```text
accept(NULL, NULL)
```

with:

```text
accept(&peer_addr, &peer_len)
+ retain peer_addr
```

It does not propose:

```text
accept()
+ unconditional separate getpeername()
```

That distinction matters when using the 2011 performance result to reason about the cost of accept-time retention.

---

## 16. Modern proof of concept and measurements

The proof-of-concept repository is:

https://github.com/bobnil/libuv-accept-peer-address-poc

Its purpose is deliberately limited. It demonstrates that:

1. peer identity is available during accept;
2. libuv can preserve it;
3. when it is preserved, the observed reset-before-peer-lookup failure mode disappears.

The patch is not intended to prescribe the final internal representation.

### 16.1 Node correctness test

A correctness reproducer was run against Node.js v26.10.0, which bundles libuv v1.52.1.

Both builds report the same Node and libuv versions; the patched build differs by the local proof-of-concept libuv
modification.

Results over 100,000 connections:

```text
Node.js v26.10.0 / libuv v1.52.1, upstream

Total:     100000
Address:   3268
Undefined: 96732


Node.js v26.10.0 / libuv v1.52.1, patched

Total:     100000
Address:   100000
Undefined: 0
```

The exact upstream failure rate is timing-dependent. The important result is that the patched build retained peer
identity in all 100,000 attempts in this test, while the upstream behavior allowed previously available peer identity to
become unrecoverable.

### 16.2 Standalone libuv performance benchmark

The performance experiment is separate from the Node correctness test.

It compares unmodified libuv v1.53.0 at commit `840404ce8ba7cc0204be52389a6cfff9f2c90fb6` with the same libuv revision
plus the POC patch.

The minimal server accepts each connection and immediately closes it. It never queries the peer address.

That intentionally gives upstream its best case:

```text
upstream:
    accept without sockaddr
    no later getpeername()

patched:
    accept with sockaddr
    retain address
    benchmark never uses it
```

The collected loopback and two-machine datasets range from 500,000 to 50 million connections per run. The direction of
the measured difference changes between datasets and run-to-run variation is larger than the observed difference.

The benchmark therefore did not identify a reproducible connection-throughput regression in the tested Linux
environment. This should not be interpreted as proof that retrieving the address costs exactly zero, nor as a claim
about other kernels, platforms, CPUs, or workloads.

**Sources**

- POC repository: https://github.com/bobnil/libuv-accept-peer-address-poc
- benchmark methodology and results: https://github.com/bobnil/libuv-accept-peer-address-poc/blob/main/BENCHMARK.md

---

## 17. What the historical record supports

### Directly supported by the sources

- The original 4.2BSD interface let `accept()` return the peer address, and contemporary Berkeley documentation
  demonstrates that use by a server. `getpeername()` was a separate operation on a connected socket; the API did not
  require a follow-up query to obtain the newly accepted peer.
- BSD-derived code checks current connected state in `getpeername()` and can return `ENOTCONN` after a connection
  previously had a known peer.
- Early Linux stored the peer on a pending child but did not yet reliably return its address after close. Linux 2.3.15
  explicitly retained an established-then-RST child for accept, and 2.3.43 differentiated accept-time address lookup
  (`peer=2`) from `getpeername()` (`peer=1`) for IPv4 and IPv6.
- Legacy Node retained remote address and port from its `accept()` result before emitting `connection`.
- The first public libuv backend already obtained a peer sockaddr on Unix but retained only the accepted fd. The
  original API separated connection notification from `uv_accept()`, and the Windows backend transferred the accepted
  socket through that API.
- `uv_tcp_getpeername()` was introduced later as a separate live query, adopted by Node, and subsequently made lazy for
  an approximately 1% reported `http_simple` gain. That benchmark did not use peer identity.
- libuv later stopped requesting the otherwise-unused sockaddr from Unix `accept()` / `accept4()`.
- The #7566 discussion confirms that the problem was understood by Node/libuv maintainers. Ben Noordhuis described the
  then-current omission of the accept-time sockaddr as intentional, cited the cost of copying frequently unused address
  data, and said that storing it in `uv_tcp_t` would at that point break the ABI. TJ Fontaine said that the absence of
  an optional `struct sockaddr` from `uv_accept()` had already been discussed, that this prevented a fix in a stable
  release, and that work was underway on a solution intended to improve the situation.
- The 2015 Node cache change helps only after a successful first peer lookup. Reports in 2018 and 2023 document the
  remaining first-access failure; current Unix libuv still uses a later live query.

### Strong technical conclusion

An address returned by `accept()` and a later `getpeername(accepted_fd)` are not semantically interchangeable after
every socket-state transition. Linux 2.3.43 explicitly permits accept-time retrieval of stored peer information in a
state where ordinary `getpeername()` returns `ENOTCONN`. The BSD interface had offered accept-time retrieval from its
inception.

For an accepted connection, discarding the accept-time peer address therefore discards information with a lifetime that
is not guaranteed to be reproduced by a later query against the socket.

### Reasonable architectural interpretation

The early Unix and Windows implementations, together with contemporary API documents, suggest an abstraction in which
libuv holds an accepted connection pending `uv_accept()` and queries endpoint information separately. No requirement in
the reviewed original BSD API explains the later live-query model. This interpretation does not establish the designers'
specific motivation.

### Not established by the sources reviewed

The research did **not** find direct evidence for:

- the exact reason peer metadata was excluded from the original `uv_accept()` state;
- whether reset-before-peer-query was considered in the original design;
- whether ABI, handle-size, performance, portability, or other concerns motivated the original 2011 decision;
- the exact implementation TJ Fontaine meant by the solution under development in May 2014;
- whether that proposed solution involved changing uv_accept(), storing peer information elsewhere, changing Node's
  behavior, or some combination of those approaches;
- a historical Unix convention requiring `accept()` followed by `getpeername()` rather than using the address returned
  by `accept()`.

## 18. Condensed chronology

| Period                         | Change                                                                                | Effect on peer-address lifetime                                                      |
| ------------------------------ | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| 4.2BSD, 1983                   | `accept()` returns the connecting peer address; `getpeername()` is a separate query   | Accept-time peer identity is part of the original interface                          |
| Linux 0.98                     | Pending child stores peer; accept address uses state-sensitive `getname()`            | Stored peer does not yet guarantee address return after close                        |
| Linux 1.1–1.3                  | Readiness/accept behavior adjusted toward BSD semantics                               | Closed-state children can be eligible, but RST/error paths still matter              |
| Linux 2.3.15                   | Established-then-RST child retained for `accept()`                                    | Dead child can be collected by accept                                                |
| Linux 2.3.43                   | Accept uses `peer=2`; ordinary `getpeername()` uses `peer=1`                          | Stored address can be returned during accept even when later query yields `ENOTCONN` |
| Pre-libuv Node                 | `accept()` result contains fd and peer; Node stores address/port before `connection`  | Peer identity persists                                                               |
| Jul 2011, Node v0.5.0          | First public libuv backend; Unix accept gets sockaddr but persists only `accepted_fd` | Accept-time peer metadata is not propagated                                          |
| Sep 2011, libuv `12b01e9`      | Adds `uv_tcp_getpeername()`                                                           | Separate live socket query becomes available                                         |
| Sep 2011, Node `e20d0c1`       | Uses query for `remoteAddress` / `remotePort`                                         | Lookup initially occurs before emitting connection                                   |
| Sep 8 2011, Node v0.5.6        | #1345 ships; libuv becomes default on Unix                                            | New model becomes the Unix default                                                   |
| Oct 2011, Node `1bb820a`       | Removes unconditional query; reports ~1% `http_simple` gain                           | Peer query becomes lazy; interval for state changes grows                            |
| May 2012, libuv `752ac30`      | Stops passing sockaddr to Unix `accept()`                                             | Removes retrieval of already-unused address                                          |
| May 2014, Node #7566           | Identifies accept/query race and suggests retaining accept information                | Root cause documented                                                                |
| 2015, Node `30666f2`           | Uses cached peer name after close                                                     | Helps only if lookup happened before close                                           |
| Oct 2018, Node #23858          | Reproduces undefined remote fields in connection handler                              | Symptom persists                                                                     |
| May 2023, Node #48061 / #48139 | Identifies first-lookup-after-destruction limitation; updates docs                    | Behavior documented without changing accept path                                     |
| Reviewed current libuv         | `accept(..., NULL, NULL)` and later `getpeername()`                                   | Underlying lifetime behavior remains                                                 |

---

## References

### §1 — Socket API baseline: BSD and Linux

#### BSD sockets origins

- 1.1 Samuel J. Leffler, Robert S. Fabry, and William N. Joy, _A 4.2bsd Interprocess Communication Primer_,
  UCB/CSD-83-145, July 1983.  
   <https://www2.eecs.berkeley.edu/Pubs/TechRpts/1983/5451.html>

- 1.2 Historical 2.11BSD `accept(2)` manual, address result parameter.  
  <https://man.freebsd.org/cgi/man.cgi?manpath=2.11+BSD&query=accept&sektion=2>

- 1.3 Historical FreeBSD `getpeername(2)` manual, 4.2BSD origin and `ENOTCONN`.  
  <https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+4.10-RELEASE+and+Ports&query=getpeername&sektion=2>

- 1.4 Early BSD-derived FreeBSD `sys/kern/uipc_syscalls.c`, accept and connected-state check in `getpeername()`.  
  <https://minnie.tuhs.org/94Web/FreeBSD-srctree/newsrc/kern/uipc_syscalls.c.html>

- 1.5 FreeBSD-net discussion, January 23, 2011, _getpeername returning ENOTCONN for a connected socket_.  
  <https://lists.freebsd.org/pipermail/freebsd-net/2011-January/027648.html>

#### Linux kernel history

- 1.6 Linux 0.98, pending child, stored peer, and state-sensitive accept address lookup.
  <https://kernelhistory.sourcentral.org/linux-0.98/>
  - `net/tcp/tcp.c` <https://kernelhistory.sourcentral.org/linux-0.98/S/254.html>
  - `net/tcp/sock.c` <https://kernelhistory.sourcentral.org/linux-0.98/S/252.html>
  - `net/socket.c` <https://kernelhistory.sourcentral.org/linux-0.98/S/232.html>

- 1.7 Linux 1.1.13 and 1.1.45, BSD accept/select concern, peer-name state checks, and accept error path.
  - Linux 1.1.13 `net/inet/tcp.c` <https://kernelhistory.sourcentral.org/linux-1.1.13/S/329.html>
  - Linux 1.1.45 `net/inet/tcp.c` <https://kernelhistory.sourcentral.org/linux-1.1.45/S/352.html>
  - Linux 1.1.13 `net/inet/af_inet.c` <https://kernelhistory.sourcentral.org/linux-1.1.13/S/353.html>
  - Linux 1.1.45 `net/inet/af_inet.c` <https://kernelhistory.sourcentral.org/linux-1.1.45/S/378.html>

- 1.8 Linux, readiness/accept criterion and closing-state refinement.
  - Linux 1.1, `patch50`: <https://www.kernel.org/pub/linux/kernel/v1.1/patch50.gz>
  - Linux 1.1, `patch63`: <https://www.kernel.org/pub/linux/kernel/v1.1/patch63.gz>

- 1.9 Linux 1.3.0, comment on BSD accept semantics and the remaining error path.
  - `net/ipv4/af_inet.c`: <https://kernelhistory.sourcentral.org/linux-1.3.0/S/528.html>

- 1.10 Linux `patch-2.3.15`, especially `net/ipv4/tcp_ipv4.c`, `tcp.c`, and `af_inet.c` — established-then-RST child
  retained for accept; compare 2.3.14 and 2.3.15 snapshots.
  - `patch-2.3.14` <https://www.kernel.org/pub/linux/kernel/v2.3/patch-2.3.14.gz>,
  - `patch-2.3.15` <https://www.kernel.org/pub/linux/kernel/v2.3/patch-2.3.15.gz>

- 1.11 Linux `patch-2.3.43`, especially `net/socket.c`, `net/ipv4/af_inet.c`, and `net/ipv6/af_inet6.c` — distinct
  `peer=2` and `peer=1` lookups; compare 2.3.42 and 2.3.43 snapshots.
  - `patch-2.3.42` <https://www.kernel.org/pub/linux/kernel/v2.3/patch-2.3.42.gz>
  - `patch-2.3.43` <https://www.kernel.org/pub/linux/kernel/v2.3/patch-2.3.43.gz>

### §2 — Before libuv: Node retained peer identity from accept

- 2.1 Node v0.5.9 legacy networking backend  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.9/lib/net_legacy.js>

### §3 — July 2011: the first public libuv backend changes the abstraction

- 3.1 Node v0.5.0 changelog  
  <https://github.com/nodejs/node/blob/main/doc/changelogs/CHANGELOG_ARCHIVE.md#20110705-version-050-unstable>

- 3.2 Node v0.5.0 bundled libuv `desired-api.md`  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/desired-api.md>

- 3.3 Node v0.5.0 bundled libuv Unix implementation  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/uv-unix.c>

### §4 — The Windows side: the same connection/handle separation

- 4.1 Node v0.5.0 bundled libuv Windows implementation  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/uv-win.c>

- 4.2 Node v0.5.0 bundled libuv Windows private fields  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/uv-win.h>

- 4.3 Ryan Dahl, `Asynchronous I/O in Windows for Unix Programmers` / `iocp-links.html`  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.0/deps/uv/iocp-links.html>

### §5 — September 2011: `uv_tcp_getpeername()` is introduced

- 5.1 libuv `12b01e95f9afb56f602ca17f44d3b7e22e37c656` — `Specialize uv_xxx_getsockname, add uv_tcp_getpeername`  
  <https://github.com/libuv/libuv/commit/12b01e95f9afb56f602ca17f44d3b7e22e37c656>

### §6 — Node adopts `uv_tcp_getpeername()` for `remoteAddress`

- 6.1 Node `e20d0c1` — `net-uv: correctly set socket.remoteAddress and -port`  
  <https://github.com/nodejs/node-v0.x-archive/commit/e20d0c1>

- 6.2 Node v0.5.6 changelog  
  <https://github.com/nodejs/node/blob/main/doc/changelogs/CHANGELOG_ARCHIVE.md#20110908-version-056-unstable>

### §7 — Initially Node queried peer identity immediately

- 7.1 Node v0.5.9 libuv networking backend  
  <https://github.com/nodejs/node-v0.x-archive/blob/v0.5.9/lib/net_uv.js>

### §8 — October 2011: the peer lookup is made lazy for performance

- 8.1 Node `1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0` — `net: remove unconditional getpeername() call`  
  <https://github.com/nodejs/node-v0.x-archive/commit/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0>

- 8.2 `http_simple.js` at `1bb820a`  
  <https://github.com/nodejs/node-v0.x-archive/blob/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0/benchmark/http_simple.js>

- 8.3 `http_simple_bench.sh` at `1bb820a`  
  <https://github.com/nodejs/node-v0.x-archive/blob/1bb820a339e64898a4b1d66cfc3e7a6d2e6b8ef0/benchmark/http_simple_bench.sh>

- 8.4 ApacheBench documentation (`-k` / KeepAlive)  
  <https://httpd.apache.org/docs/current/en/programs/ab.html>

### §9 — May 2012: stop requesting the unused sockaddr

- 9.1 libuv `752ac30ec820bc0ef4dfea698fd7119a8a4aa14c` — `unix: don't pass sockaddr to accept()`  
  <https://github.com/libuv/libuv/commit/752ac30ec820bc0ef4dfea698fd7119a8a4aa14c>

### §10 — 2014: Node issue #7566 identifies the same root cause and discusses a fix

- 10.1 Node v0.x issue #7566 — `Race condition when getting remoteAddress of connection`  
  <https://github.com/nodejs/node-v0.x-archive/issues/7566>

- 10.2 Ben Noordhuis comment, May 6, 2014.  
  <https://github.com/nodejs/node-v0.x-archive/issues/7566#issuecomment-42294224>

- 10.3 TJ Fontaine comment, May 6, 2014.  
  <https://github.com/nodejs/node-v0.x-archive/issues/7566#issuecomment-42307064>

### §11 — 2015: preserving an already-cached peer name

- 11.1 Node v0.x issue #9287 — `socket.remoteAddress after close is undefined`  
  <https://github.com/nodejs/node-v0.x-archive/issues/9287>

- 11.2 Node v0.x PR #9366  
  <https://github.com/nodejs/node-v0.x-archive/pull/9366>

- 11.3 Node `30666f2` — `net: use cached peername to resolve remote fields`  
  <https://github.com/nodejs/node-v0.x-archive/commit/30666f2>

### §12 — 2018: the original symptom is still reproducible

- 12.1 Node issue #23858 — `socket.remoteAddress is sometimes undefined in net server connect handler`  
  <https://github.com/nodejs/node/issues/23858>

### §13 — 2023: the first-access-after-destruction limitation is rediscovered

- 13.1 Node issue #48061 — `Closed socket leads to undefined values in socket.remote* properties`  
  <https://github.com/nodejs/node/issues/48061>

- 13.2 Node PR #48139 — `doc: update socket.remote* properties documentation`  
  <https://github.com/nodejs/node/pull/48139>

- 13.3 Node landed commit on main, `5275843`.  
  <https://github.com/nodejs/node/commit/5275843>

### §14 — Current libuv: the architecture remains recognizable

- 14.1 Current Unix accept helper  
  <https://github.com/libuv/libuv/blob/v1.x/src/unix/core.c>

- 14.2 Current Unix server accept / `accepted_fd` path  
  <https://github.com/libuv/libuv/blob/v1.x/src/unix/stream.c>

- 14.3 Current Unix `uv_tcp_getpeername()` implementation  
  <https://github.com/libuv/libuv/blob/v1.x/src/unix/tcp.c>

- 14.4 libuv TCP API documentation  
  <https://docs.libuv.org/en/v1.x/tcp.html>

### §16 — Modern proof of concept and measurements

- 16.1 POC repository  
  <https://github.com/bobnil/libuv-accept-peer-address-poc>

- 16.2 POC benchmark methodology and results  
  <https://github.com/bobnil/libuv-accept-peer-address-poc/blob/main/BENCHMARK.md>
