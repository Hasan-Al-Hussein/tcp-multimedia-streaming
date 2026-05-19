# Source Code

This directory contains the core implementation of the multimedia streaming system.

## Files

### Media_server.c
Multithreaded TCP media server responsible for:
- Client connection handling
- Media catalog management
- Real-time audio/video streaming
- Thread management
- Stream control logic

### Media_client.c
Client-side streaming application responsible for:
- Server communication
- Media selection interface
- SDL video playback
- FFmpeg/ffplay audio playback
- Stream interruption handling

## Core Concepts

- TCP socket programming
- Multithreading
- Real-time byte streaming
- Application-layer protocol design
- Media playback integration
