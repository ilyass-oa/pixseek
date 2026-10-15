# TCP protocol

The client connects using IPv4. The default listener is `127.0.0.1:5555`.

## Request and response

A request contains a 4-byte unsigned payload length in network byte order, followed by exactly that many BMP bytes. Lengths outside **1–4,194,304 bytes** close the connection before allocating a payload buffer. TCP reads and writes may split anywhere in a frame.

The response is UTF-8 text terminated by a NUL byte, at most 4096 bytes including that terminator:

```text
Most similar image found: 'img/1.bmp' with a distance of 0.
```

An unsupported or malformed image receives:

```text
No similar image found (no comparison could be performed successfully).
```

A connection supports multiple requests, including pipelined frames; replies retain request order. Only one search per connection runs at a time. A client may shut down its write side after a complete request and still receive the response. EOF inside a frame discards it. Closing a client never transfers a result to a newly accepted connection.

## Catalog and comparison

The catalog contains regular `.bmp` files directly inside `--images`; extensions are case-insensitive and symlinks are skipped. At most 10,000 candidate paths are accepted. Invalid files are skipped, and an empty usable catalog prevents startup. Paths are sorted; the first path wins equal-distance ties.

The C library reduces the image to 32×32 grayscale pixels, applies a discrete cosine transform and produces a 64-bit perceptual hash. Hamming distance ranges from 0 to 64. The nearest result is returned without a confidence threshold; a zero distance does not prove identical files.

BMP support is limited to uncompressed 24-bit BGR and 32-bit BGRX, one plane, positive widths, and either row orientation. Row padding, offsets and dimensions are checked. Alpha is ignored. Limits are 4 MiB per file, 4096 pixels per side and 1,048,576 pixels total. See Microsoft's [image header](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader) and [file header](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapfileheader) documentation.

## Resource limits

Defaults are 4 search workers and 32 active clients. `--workers` accepts 1–16 and `--max-clients` accepts 1–64. Outstanding jobs, including results awaiting delivery, are separately capped at the client limit; excess work or connections are closed. Each receiving connection and queued job holds at most one bounded image.

`--timeout-ms` defaults to 5000. The server uses an absolute deadline from acceptance or the previous completed response through receipt, search and response delivery. Trickle traffic does not extend it. The client applies an absolute timeout to each connect/send/receive exchange. The server joins its workers and closes sockets on SIGINT or SIGTERM.

This is a trusted-network protocol with no TLS or authentication.
