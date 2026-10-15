# Pixseek

A C++ client/server project that finds visually similar BMP images over TCP. A small C library computes a 64-bit perceptual hash; the server compares hashes using Hamming distance and returns the closest image.

## Run

Requires Linux, a C11/C++17 compiler, Make and Python 3 for tests.

```sh
make -j2
./build/pixseek-server --images img
```

In another terminal:

```sh
./build/pixseek-client img/1.bmp tests/images/1-1.bmp
```

The client also accepts one file path per line on standard input:

```sh
printf '%s\n' img/1.bmp img/6.bmp | ./build/pixseek-client
```

Both programs support `--port` and `--timeout-ms`; use `--help` for all options. The default address is `127.0.0.1:5555`.

## How it works

The server indexes the catalog once, uses `poll` for connections and dispatches searches to a fixed worker pool. Each connection processes requests in order. Partial messages, disconnects, invalid lengths and timeouts are handled explicitly.

Supported inputs are uncompressed 24/32-bit BMP files up to 4 MiB, 4096 pixels per side and 1,048,576 pixels total. Perceptual hashes measure visual similarity, not file integrity. The protocol has no authentication or encryption and is intended for local or trusted networks.

## Tests

```sh
make test
make test CC=clang CXX=clang++ BUILD=build-sanitize SANITIZE=1
```

Tests cover BMP decoding, concurrent clients, fragmented messages, malformed input, disconnects, resource cleanup and shutdown. CI runs both builds with memory and undefined-behavior checks.

See [the protocol](docs/protocol.md) for message framing and limits.
