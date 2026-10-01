# Hash-Cracking TCP Server

A small concurrent TCP server that searches an integer range for a value whose SHA-256 digest matches a supplied hash.

Each client is handled by a child process. The server permits up to four active children at a time and reaps completed children before accepting further work.

## Requirements

- A C compiler with POSIX socket and process support
- `make`

The project has been tested on macOS and Linux.

## Build

```sh
make
```

This produces the `server` executable.

## Run

Start the server on its default port, `5003`:

```sh
./server
```

Or provide a port number:

```sh
./server 8080
```

The server listens on all network interfaces and accepts TCP connections.

## Protocol

All multi-byte integer fields use big-endian byte order.

### Request

The client must send exactly 49 bytes:

| Bytes | Field | Description |
| --- | --- | --- |
| 0-31 | `hash` | Target SHA-256 digest (32 bytes) |
| 32-39 | `start` | Inclusive start of the search range (`uint64_t`) |
| 40-47 | `end` | Exclusive end of the search range (`uint64_t`) |
| 48 | `priority` | Priority byte; received but not currently used by the server |

For each integer $i$ in the range $[start, end)$, the server computes the SHA-256 hash of the in-memory `uint64_t` value and compares it with `hash`.

### Response

The server returns one 8-byte, big-endian `uint64_t`:

- The matching value, when one is found.
- `0`, when no match is found in the requested range.

## Project Files

- `server.c`: TCP server, request handling, child-process concurrency, and brute-force search.
- `messages.h`: Protocol sizes, field offsets, and endian helpers for macOS/Linux.
- `lonesha256.h`: Bundled single-file SHA-256 implementation.
- `Makefile`: Build rule for the server executable.
