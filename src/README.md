# Source implementation

The repository contains the complete C implementation used by the project.

## `Media_server.c`

- Persistent TCP server on port `9000`.
- Detached pthread worker per accepted client.
- Mutex-protected client-ID allocation.
- Per-session category and file-selection state.
- `SIZE <bytes> <safe_name>` header followed by an exact-length byte stream.
- Complete-write handling, throttled transfer, and mid-stream `ABORT` detection.

## `Media_client.c`

- Persistent interactive TCP client.
- Line/header parsing followed by fixed-length stream reception.
- Simultaneous file persistence and playback through Unix pipes.
- ffplay audio and mpv video subprocess integration.
- `select()`-driven socket/terminal controls, pause/resume, abort, and cleanup.

## Build

```bash
gcc -std=c11 -Wall -Wextra -pthread Media_server.c -o media_server
gcc -std=c11 -Wall -Wextra Media_client.c -o media_client
```

The client intentionally delegates decoding to FFmpeg/ffplay or mpv, so no SDL development library is required for compilation.
