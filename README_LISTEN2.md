# Node/libuv `uv_listen2()` proof of concept

This note describes the second proof of concept in this repository: a libuv API
shape that delivers the TCP peer address captured by `accept()`, and a Node.js
integration that uses that address for `net.Socket` peer information.

The background and motivation are documented in:

- `HISTORICAL_CONTEXT.md`
- libuv issue: https://github.com/libuv/libuv/issues/5308
- Node.js issue: https://github.com/nodejs/node/issues/66422

The relevant patch files are:

- `poc-libuv-listen2.patch`: standalone libuv proof of concept
- `poc-node-listen2.patch`: Node.js source changes only
- `poc-node-uv-listen2.patch`: combined Node.js patch, including bundled libuv

The Node.js patch was made against commit
`fbbf534de78f8c4b2eca4f8e86c6af87939fb9d6`, reported by the test build as
`v27.0.0-pre`. The bundled libuv version is `v1.52.1`.

## 1. Changes in libuv

The libuv POC adds a new listener API, tentatively named `uv_listen2()`:

```c
typedef void (*uv_connection2_cb)(
    uv_stream_t* server,
    int status,
    const struct sockaddr* peer,
    socklen_t peerlen);

UV_EXTERN int uv_listen2(
    uv_stream_t* stream,
    int backlog,
    uv_connection2_cb cb);
```

The important semantic difference from `uv_listen()` is that the callback
receives the peer address returned by the underlying accept operation.

On Unix, the existing `uv_listen()` path is kept on the current fast path:

```c
accept(fd, NULL, NULL)
```

Only streams using `uv_listen2()` ask the kernel for the peer address:

```c
struct sockaddr_storage peer;
socklen_t peerlen = sizeof(peer);

accept(fd, (struct sockaddr*) &peer, &peerlen)
```

The accepted file descriptor is still stored in libuv's existing pending-accept
state, and `uv_accept(server, client)` still works as before. The peer address
is not stored persistently inside libuv. It is stack-local in the accept path
and is valid only during the `uv_connection2_cb` call. Applications that need
the address after the callback must copy it themselves.

The POC deliberately avoids adding sockaddr fields to `uv_stream_t`, `uv_tcp_t`,
or other public handle layouts. It uses an internal TCP handle flag to select
the `uv_listen2()` accept path. On Windows, the POC exposes the symbol but
returns `UV_ENOSYS`; the Unix path is the part being tested here.

This means the intended performance split is:

```text
uv_listen():
    accept(NULL, NULL)

uv_listen2():
    accept(&peer, &peerlen)
    callback receives peer
```

The purpose is to test an API shape where applications that do not request
accept-time peer identity continue to pay no additional sockaddr-copying cost.

## 2. Changes in Node.js

The Node.js POC changes TCP server listens to use the new libuv callback on
Unix. Pipes and Windows TCP listens continue to use the existing `uv_listen()`
path.

The main Node.js changes are:

1. `TCPWrap::Listen()` calls `uv_listen2()` on Unix.

2. `ConnectionWrap<TCPWrap, uv_tcp_t>` gets an `OnConnection2()` callback. It
   follows the same flow as the existing `OnConnection()` callback:

   ```text
   libuv connection callback
       -> create accepted TCPWrap
       -> uv_accept(server, client)
       -> emit JS onconnection callback
   ```

   The additional step is that, after a successful `uv_accept()`, Node copies
   the peer address from the `uv_listen2()` callback onto the accepted
   `TCPWrap`.

3. `TCPWrap` stores the copied accept-time peer address in its own state:

   ```text
   sockaddr_storage accepted_peername_
   size_t accepted_peername_len_
   ```

   This keeps long-term ownership in Node.js, not in libuv.

4. The TCP `getpeername` binding is changed from the generic
   `GetSockOrPeerName<TCPWrap, uv_tcp_getpeername>` helper to a TCP-specific
   `TCPWrap::GetPeerName()`.

   That method first checks whether the accepted handle has a stored
   accept-time peer address. If it does, that address is returned to JavaScript.
   If not, it falls back to `uv_tcp_getpeername()` exactly as before.

   This preserves existing behavior for outbound/client TCP sockets and any
   TCPWrap that was not created through `uv_listen2()`.

5. The stored peer address is also carried through `TCPWrap::TransferData`, so
   a socket transferred to another worker does not lose the accept-time address
   before JavaScript has read `remoteAddress`.

No JavaScript `net.js` API change is needed. Existing properties such as
`socket.remoteAddress`, `socket.remotePort`, and `socket.remoteFamily` already
go through the C++ handle's `getpeername(out)` binding. The POC changes the data
source used by that binding, not the JavaScript API.

## Result

The relevant Node.js result is recorded in `results/node-listen2-results.csv`:

```csv
variant,node,libuv,total,success,failure,success_rate,failure_rate
listen2,v27.0.0-pre,v1.52.1,100000,100000,0,100.0,0.0
```

In this run, all 100,000 accepted reset-before-query connections retained a
peer address through the `uv_listen2()` path.

This is a proof of concept, not a proposed final production patch. The goal is
to demonstrate that current Node.js can preserve accept-time TCP peer identity
when libuv provides it, while leaving the existing `uv_listen()` behavior and
JavaScript API shape intact.
