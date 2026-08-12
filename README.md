<div align="center">
  <img src="assets/repository-banner.svg" width="100%" alt="Multithreaded TCP multimedia streaming system" />
  <br /><br />
  <a href="https://github.com/Hasan-Al-Hussein"><img src="https://img.shields.io/badge/ENGINEERING_PORTFOLIO-0F172A?style=for-the-badge&logo=github&logoColor=white" alt="Back to Hasan Al Hussein's engineering portfolio" /></a>
  <img src="https://img.shields.io/badge/C-POSIX-00599C?style=for-the-badge&logo=c&logoColor=white" alt="C and POSIX" />
  <img src="https://img.shields.io/badge/TCP%2FIP-PERSISTENT_SESSION-2563EB?style=for-the-badge" alt="Persistent TCP/IP session" />
  <img src="https://img.shields.io/badge/PTHREADS-CONCURRENT-6F42C1?style=for-the-badge" alt="Concurrent pthread server" />
</div>

# TCP Multimedia Streaming System

**A concurrent C client-server system that streams audio and video across routed TCP/IP networks through a persistent application protocol.**

The project goes beyond file transfer: the server maintains a stateful media session, supports concurrent clients, and handles partial socket writes and mid-stream aborts. The client receives and saves each stream while piping it into FFmpeg/ffplay or mpv for immediate playback, with pause, resume, exit, and player-failure recovery.

<p align="center">
  <img src="images/network_topology.png" width="1100" alt="Routed TCP multimedia network topology" />
</p>

## Project snapshot

| Area | Implementation |
|---|---|
| Core | More than 1,700 lines of C across client and server |
| Server | POSIX sockets, thread-per-client concurrency, pthread mutex, persistent sessions |
| Protocol | Line-delimited commands plus `SIZE <bytes> <safe_name>` framing and raw media bytes |
| Client I/O | `select()` multiplexing across the TCP socket and terminal controls |
| Playback | Audio through FFmpeg/ffplay; video through mpv with low-latency options |
| Reliability | Complete-write loops, disconnect handling, abort signaling, process and terminal cleanup |
| Network | Multi-subnet Cisco topology with static routing |
| Validation | Wireshark packet analysis, router CLI checks, and end-to-end media playback |

## Architecture

<p align="center">
  <img src="images/system_architecture.png" width="1100" alt="TCP multimedia system architecture" />
</p>

```text
Interactive client
  -> persistent TCP session on port 9000
     -> category / file selection protocol
        -> thread-per-client server
           -> SIZE header + throttled media byte stream
              -> client file sink + playback pipe
                 -> ffplay (audio) / mpv (video)

Control path: terminal input -> select() -> pause/resume or ABORT
Network path: client subnet -> Cisco routing -> server subnet
```

The server owns the catalog and one menu-state machine per client. A mutex protects only shared client-ID allocation; each detached worker thread otherwise owns its socket and session state. This keeps concurrent sessions isolated without serializing the streaming path.

## Application protocol

The protocol separates newline-delimited control messages from fixed-length media bodies:

```text
server -> client: menu lines ... END\n
client -> server: <category | file | back | exit>\n
server -> client: SIZE <byte_count> <sanitized_filename>\n
server -> client: exactly <byte_count> raw bytes
client -> server: ABORT\n       (optional during transfer)
```

Important protocol details:

- `send_all()` retries partial writes until every header or menu byte is transmitted.
- The file size gives the receiver an exact message boundary on a byte-stream transport.
- Filenames are sanitized before inclusion in the wire header.
- `select()` lets the server observe abort commands during transfer and lets the client react to both socket and keyboard input.
- One connection can browse and play multiple titles without reconnecting.

## Concurrent server

`src/Media_server.c` implements:

- socket creation, `SO_REUSEADDR`, bind, listen, and a continuous accept loop;
- a detached POSIX thread for every accepted client;
- mutex-protected assignment of unique client IDs;
- per-client category and file-selection state;
- audio/video type detection and file-size discovery;
- chunked, throttled streaming with abort and disconnect detection; and
- deterministic socket, file, thread-context, and session cleanup.

## Streaming client

`src/Media_client.c` implements:

- a persistent interactive TCP connection;
- deterministic parsing of menus and `SIZE` headers;
- simultaneous download and playback through a process pipe;
- FFmpeg/ffplay audio playback and mpv video playback;
- raw-terminal controls: `SPACE` pauses/resumes video and `E` aborts the stream;
- `SIGSTOP`/`SIGCONT` playback control plus graceful `SIGTERM` and bounded `SIGKILL` fallback;
- `SIGPIPE`, `EPIPE`, player-window closure, interrupted calls, and server disconnect handling; and
- restoration of the terminal and all file descriptors on success, abort, or failure.

## Routed-network validation

<p align="center">
  <img src="images/client_streaming_demo.png" width="48%" alt="Client receiving and playing a media stream" />
  <img src="images/wireshark_validation.png" width="48%" alt="Wireshark validation of TCP media traffic" />
</p>

The system was exercised through two Cisco routers and multiple Linux subnets with static routes. Validation included:

- end-to-end reachability and multi-hop routing;
- TCP handshake, sequence and acknowledgement behavior;
- persistent session continuity during multiple selections;
- stream traffic and retransmission inspection in Wireshark;
- graceful completion, client abort, and session termination; and
- audio and video playback at the receiving endpoint.

## Build and run

Requirements: Linux or another POSIX environment, GCC, pthreads, FFmpeg/ffplay, and mpv.

```bash
gcc -std=c11 -Wall -Wextra -pthread src/Media_server.c -o media_server
gcc -std=c11 -Wall -Wextra src/Media_client.c -o media_client

./media_server
./media_client <server-ip>
```

Before running, replace the demonstration paths in `media_files[]` with media files available on the server. Both programs use TCP port `9000`.

## Repository map

```text
src/
  Media_server.c   concurrent catalog and streaming server
  Media_client.c   interactive receiver and playback client
images/
  network topology, architecture, demo, and Wireshark evidence
docs/
  tcp_multimedia_streaming_report.pdf
```

## Engineering tradeoffs and limitations

- TCP provides ordered reliable delivery but can increase latency under loss through head-of-line blocking.
- The protocol is intentionally small and readable; it does not currently include version negotiation, checksums, authentication, or encryption.
- Media paths, port, buffer size, and throttling are compile-time configuration rather than runtime options.
- Thread-per-client handling is appropriate for this lab scale; a production service would add bounded concurrency, backpressure, observability, and load testing.
- FFmpeg/mpv are local playback dependencies rather than embedded codecs.

## Skills demonstrated

`C` · `POSIX sockets` · `TCP/IP` · `pthreads` · `select()` · `process control` · `signals` · `pipes` · `FFmpeg` · `mpv` · `Linux` · `Cisco routing` · `Wireshark`

## Documentation

- [Full technical report](docs/tcp_multimedia_streaming_report.pdf)
- [Source implementation notes](src/README.md)

---

Built by **Hasan Al Hussein** at Khalifa University.
