// media_client.c - stays connected, can play multiple files, "exit" to quit
// Uses FFmpeg (ffplay) for playback - optimized for low-latency streaming
//
// This client connects to a media server and can stream/play multiple media files
// in a single session. It supports both audio and video playback with low-latency
// streaming using pipes. The client maintains a persistent connection and displays
// a menu-driven interface for selecting media files.

// Standard I/O and memory management
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// POSIX system calls
#include <unistd.h>
// Network socket operations
#include <arpa/inet.h>
// Signal handling for process control
#include <signal.h>
// Process and wait operations
#include <sys/types.h>
#include <sys/wait.h>
// Time operations for select() timeout
#include <sys/time.h>
// I/O multiplexing for non-blocking input handling
#include <sys/select.h>
// Terminal control for raw input mode
#include <termios.h>
// Error handling
#include <errno.h>
// String comparison functions (case-insensitive)
#include <strings.h>
// File control operations (for pipe buffer size)
#include <fcntl.h>

// F_SETPIPE_SZ is Linux-specific, define it if not available
// This constant is used to set pipe buffer sizes for better streaming performance
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

// Network configuration
#define PORT 9000                    // Server port number
#define BUF_SIZE 65536               // 64KB buffer for better video throughput
                                     // Larger buffer reduces system call overhead

// Feature flag (currently unused but defined for future use)
#define CACHE_STREAM_LOCALLY 1       // Always cache streams to local files

// Receives a line of text from a socket, reading character by character until
// a newline is encountered or the buffer is full. Used for protocol messages.
// Returns: number of bytes read (excluding null terminator), 0 on EOF, -1 on error
static ssize_t recv_line(int fd, char *buf, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);  // Read one character at a time
        if (n < 0) {
            perror("recv");
            return -1;
        }
        if (n == 0) {
            // EOF: return 0 if no data read, otherwise return what we have
            if (i == 0) return 0;
            break;
        }
        buf[i++] = c;
        if (c == '\n') break;  // Line complete
    }
    buf[i] = '\0';  // Null-terminate the string
    return (ssize_t)i;
}

// Restores the terminal to its original settings after raw mode.
// This is important to ensure the terminal works normally after playback ends.
static void restore_terminal(const struct termios *saved, int active) {
    if (active) {
        tcsetattr(STDIN_FILENO, TCSANOW, saved);  // Restore immediately
    }
}

// Enables raw terminal mode for immediate keypress detection without Enter.
// Saves the original terminal settings so they can be restored later.
// Returns: 1 if successful, 0 if terminal is not available or setup failed
static int enable_raw_terminal(struct termios *saved) {
    if (!isatty(STDIN_FILENO)) {
        return 0;  // Not a terminal, can't set raw mode
    }
    if (tcgetattr(STDIN_FILENO, saved) != 0) {
        return 0;  // Failed to get current settings
    }
    struct termios raw = *saved;
    // Disable canonical mode (line buffering) and echo
    raw.c_lflag &= ~(ICANON | ECHO);
    // Set minimum characters to 0 (non-blocking read)
    raw.c_cc[VMIN] = 0;
    // Set timeout to 0 (return immediately)
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return 0;  // Failed to set raw mode
    }
    return 1;
}

// Helper to re-apply raw terminal mode if it gets disturbed (used by mpv video path).
// Some video players (like mpv) may change terminal settings, so we periodically
// re-apply raw mode to ensure keyboard input continues to work correctly.
static void ensure_raw_terminal(const struct termios *saved) {
    if (!saved) return;
    if (!isatty(STDIN_FILENO)) return;

    struct termios raw = *saved;
    // Re-apply raw mode settings
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Launches ffplay for audio playback, reading from a pipe (read_fd).
// The pipe is connected to STDIN of ffplay so it can stream audio data.
// Returns: process ID of the child process, or -1 on error
static pid_t launch_ffplay_from_pipe(const char *title_hint, int read_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(read_fd);
        return -1;
    }
    if (pid == 0) {
        // Child process: redirect pipe to stdin and launch ffplay
        if (dup2(read_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }
        close(read_fd);  // Close original fd (stdin now has it)
        // FFmpeg ffplay with low-latency options for audio streaming
        // Start with basic working arguments, then add optimizations
        execlp("ffplay", "ffplay",
               "-nodisp",           // No display window (audio only)
               "-autoexit",         // Exit when done
               "-loglevel", "warning", // Show warnings to debug
               "-fflags", "nobuffer", // No buffering for low latency
               "-", (char *)NULL);   // Read from stdin (the pipe)
        perror("execlp");
        _exit(EXIT_FAILURE);
    }
    // Parent process: close read end (child has it now)
    close(read_fd);
    return pid;
}

// Waits for the media player process to complete and reports how it exited.
// Handles interruptions (EINTR) and reports both normal exits and signal terminations.
static void wait_for_player_completion(pid_t player_pid) {
    if (player_pid <= 0) {
        return;  // Invalid PID, nothing to wait for
    }

    int status = 0;
    // Wait for the process to exit, retrying if interrupted by a signal
    while (waitpid(player_pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;  // Interrupted by signal, try again
        }
        perror("waitpid");
        return;
    }

    // Report how the process exited
    if (WIFEXITED(status)) {
        printf("Playback finished (exit code %d).\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("Playback stopped (signal %d).\n", WTERMSIG(status));
    }
}

// Determines if a filename represents a video file based on its extension.
// Checks for common video formats: mp4, mkv, mov, avi, webm, flv.
// Returns: 1 if video file, 0 otherwise
static int is_video_file(const char *filename) {
    if (!filename) return 0;
    const char *dot = strrchr(filename, '.');  // Find last dot (extension)
    if (!dot) return 0;
    dot++;  // Move past the dot to the extension
    // Case-insensitive comparison of common video extensions
    if (strcasecmp(dot, "mp4") == 0 || strcasecmp(dot, "mkv") == 0 ||
        strcasecmp(dot, "mov") == 0 || strcasecmp(dot, "avi") == 0 ||
        strcasecmp(dot, "webm") == 0 || strcasecmp(dot, "flv") == 0) {
        return 1;
    }
    return 0;
}

// Launches ffplay for video playback with format-specific optimizations.
// Detects the video format from the filename and passes appropriate format hints
// to ffplay to improve codec detection and reduce latency.
// Returns: process ID of the child process, or -1 on error
static pid_t launch_ffplay_for_video(const char *fifo_path, int read_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(read_fd);
        return -1;
    }
    if (pid == 0) {
        // Child process: redirect pipe to stdin and launch ffplay
        if (dup2(read_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }
        close(read_fd);
        // FFmpeg ffplay with optimized low-latency settings for video streaming
        // Add format hints to help codec detection
        const char *format = "mp4";  // Default, will auto-detect if wrong
        if (fifo_path) {
            const char *ext = strrchr(fifo_path, '.');
            // Map file extensions to FFmpeg format names
            if (ext && strcasecmp(ext, ".mkv") == 0) format = "matroska";
            else if (ext && strcasecmp(ext, ".avi") == 0) format = "avi";
            else if (ext && strcasecmp(ext, ".mov") == 0) format = "mov";
        }
        execlp("ffplay", "ffplay",
               "-autoexit",         // Exit when done
               "-loglevel", "warning", // Show warnings to debug
               "-f", format,        // Force format (helps codec detection)
               "-fflags", "nobuffer", // No buffering for low latency
               "-framedrop",        // Drop frames if behind schedule
               "-", (char *)NULL);   // Read from stdin (the pipe)
        perror("execlp");
        _exit(EXIT_FAILURE);
    }
    // Parent process: close read end (child has it now)
    close(read_fd);
    return pid;
}

// mpv-based launcher for video playback (used only for video, audio stays on ffplay).
// mpv is an alternative video player that may provide better performance than ffplay
// for some video formats. Configured with ultra-low-latency settings.
// Returns: process ID of the child process, or -1 on error
static pid_t launch_mpv_for_video(const char *fifo_path, int read_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(read_fd);
        return -1;
    }
    if (pid == 0) {
        // Child process: redirect pipe to stdin and launch mpv
        if (dup2(read_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }
        close(read_fd);
        // mpv with ultra-low-latency video settings; terminal input disabled
        // so the client can handle keyboard input instead
        execlp("mpv", "mpv",
               "--no-input-terminal",           // Let client handle keyboard
               "--cache=no",                    // No caching (lowest latency)
               "--demuxer-lavf-o=fflags=+nobuffer", // No buffering in demuxer
               "--framedrop=decoder",           // Drop frames if behind schedule
               "--audio-buffer=0.1",            // Small audio buffer (100ms)
               "--vd-lavc-threads=1",           // Single thread (lower latency)
               "--quiet",                       // Quiet mode (less output)
               "--really-quiet",                // Really quiet (minimal output)
               "-", (char *)NULL);              // Read from stdin (the pipe)
        perror("execlp");
        _exit(EXIT_FAILURE);
    }
    // Parent process: close read end (child has it now)
    close(read_fd);
    return pid;
}

// Main streaming function for audio and video (using ffplay).
// Streams media data from the server while simultaneously:
// 1. Writing to a local file (caching)
// 2. Writing to a pipe for real-time playback
// 3. Handling user input to stop playback early
// Returns: 0 on success, -1 on error
static int stream_or_download_media(int sockfd, long long size, const char *title_hint) {
    int is_video = is_video_file(title_hint);
    
    // Open file for caching the stream locally
    FILE *fp = NULL;
    if (title_hint && *title_hint) {
        fp = fopen(title_hint, "wb");
        if (!fp) {
            perror("fopen");
            return -1;
        }
    } else {
        fprintf(stderr, "Error: No filename provided.\n");
        return -1;
    }

    // Set up pipe for streaming to media player
    int pipefd[2];
    int pipe_write_fd = -1;
    pid_t player_pid = -1;
    int streaming_enabled = 0;

    // Use pipe for both audio and video - works much better than file streaming
    // Pipes provide better flow control and lower latency than file-based streaming
    if (pipe(pipefd) == 0) {
        // Increase pipe buffer size for better video streaming (especially important for video)
        // Default is usually 64KB, we'll try to increase it to 2MB for video
        // Larger buffers help smooth out network jitter and prevent underruns
        int pipe_size = is_video ? (2 * 1024 * 1024) : (512 * 1024);  // 2MB for video, 512KB for audio
        if (fcntl(pipefd[0], F_SETPIPE_SZ, pipe_size) < 0) {
            // If setting fails, that's okay - use default
        }
        if (fcntl(pipefd[1], F_SETPIPE_SZ, pipe_size) < 0) {
            // If setting fails, that's okay - use default
        }
        
        // Keep blocking writes - ensures all data gets through
        // Non-blocking can cause data loss which leads to early termination
        
        pipe_write_fd = pipefd[1];  // Parent writes to this end
        if (is_video) {
            // For video: Use FFmpeg with low-latency settings
            player_pid = launch_ffplay_for_video(title_hint, pipefd[0]);
            if (player_pid > 0) {
                streaming_enabled = 1;
                printf("Streaming video via FFmpeg (low-latency mode)...\n");
            } else {
                close(pipefd[1]);
                pipe_write_fd = -1;
            }
        } else {
            // For audio: Use FFmpeg with low-latency settings
            player_pid = launch_ffplay_from_pipe(title_hint, pipefd[0]);
            if (player_pid > 0) {
                streaming_enabled = 1;
                printf("Streaming audio via FFmpeg (low-latency mode)...\n");
            } else {
                close(pipefd[1]);
                pipe_write_fd = -1;
            }
        }
    } else {
        perror("pipe");
        streaming_enabled = 0;
    }

    long long remaining = size;
    char buffer[BUF_SIZE];

    // If streaming failed and it's not a video, just download to file
    // (Videos always attempt streaming since download-only isn't useful for playback)
    if (!streaming_enabled && !is_video) {
        printf("Streaming unavailable. Downloading...\n");
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
            ssize_t r = recv(sockfd, buffer, chunk, 0);
            if (r <= 0) {
                if (r < 0) perror("recv");
                else printf("Server closed connection unexpectedly.\n");
                fclose(fp);
                return -1;
            }
            fwrite(buffer, 1, (size_t)r, fp);
            remaining -= r;
        }
        fclose(fp);
        printf("Download complete. Saved as '%s'.\n", title_hint);
        return 0;
    }

    // Set up terminal for non-blocking keyboard input
    struct termios old_term;
    memset(&old_term, 0, sizeof(old_term));
    int term_modified = enable_raw_terminal(&old_term);
    if (term_modified) {
        printf("Press 's' anytime to stop playback early.\n");
        fflush(stdout);
    }

    // Playback control state
    int playback_active = 1;
    int abort_sent = 0;

    // Main streaming loop: receive data from server and write to both file and pipe
    while (remaining > 0 && playback_active) {
        // Set up select() to monitor both socket and keyboard input
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);  // Monitor socket for incoming data
        int maxfd = sockfd;

        if (term_modified) {
            FD_SET(STDIN_FILENO, &rfds);  // Monitor keyboard for user input
            if (STDIN_FILENO > maxfd) {
                maxfd = STDIN_FILENO;
            }
        }

        // 200ms timeout - allows periodic checks without blocking too long
        struct timeval tv = {0, 200000};
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, try again
            }
            perror("select");
            break;
        }

        // Check for user input to stop playback
        if (term_modified && FD_ISSET(STDIN_FILENO, &rfds)) {
            char ch;
            ssize_t r = read(STDIN_FILENO, &ch, 1);
            if (r > 0 && (ch == 's' || ch == 'S')) {
                if (playback_active && !abort_sent) {
                    printf("\nStopping playback and requesting server to stop streaming...\n");
                    fflush(stdout);
                    playback_active = 0;
                    abort_sent = 1;
                    
                    // Close pipe first to stop data flow to player
                    if (pipe_write_fd >= 0) {
                        close(pipe_write_fd);
                        pipe_write_fd = -1;
                    }
                    
                    // Send abort command to server to stop sending data
                    if (send(sockfd, "ABORT\n", 6, 0) < 0) {
                        perror("send abort");
                    }
                    
                    // Forcefully terminate ffplay player process
                    if (player_pid > 0) {
                        // Try SIGTERM first (gentle termination)
                        if (kill(player_pid, SIGTERM) != 0 && errno != ESRCH) {
                            perror("kill SIGTERM");
                        } else {
                            // Wait a bit to see if it exits gracefully
                            usleep(100000);  // 100ms
                            // Check if still running, if so use SIGKILL (forceful)
                            if (kill(player_pid, 0) == 0) {
                                // Process still exists, force kill
                                if (kill(player_pid, SIGKILL) != 0 && errno != ESRCH) {
                                    perror("kill SIGKILL");
                                }
                            }
                        }
                    }
                    break;  // Exit streaming loop
                }
            }
        }

        // If no data available on socket, continue to next iteration
        if (!FD_ISSET(sockfd, &rfds)) {
            continue;
        }

        // Receive data from server
        size_t chunk = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
        ssize_t r = recv(sockfd, buffer, chunk, 0);
        if (r < 0) {
            perror("recv");
            break;
        }
        if (r == 0) {
            printf("Server closed connection unexpectedly during stream.\n");
            break;
        }

        remaining -= r;  // Update remaining bytes counter

        // Always write to file for caching (so user can replay later)
        if (fp) {
            size_t written = fwrite(buffer, 1, (size_t)r, fp);
            if (written < (size_t)r && ferror(fp)) {
                perror("fwrite");
                fclose(fp);
                fp = NULL;  // Don't try to write again if file error
            }
        }

        // Write to pipe for streaming (both audio and video)
        // Blocking writes ensure all data gets through - pipe buffer handles flow control
        // The pipe buffer absorbs temporary rate differences between network and player
        if (pipe_write_fd >= 0) {
            ssize_t sent = 0;
            // Write in a loop to handle partial writes (though blocking write should be complete)
            while (sent < r) {
                ssize_t w = write(pipe_write_fd, buffer + sent, (size_t)(r - sent));
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;  // Interrupted by signal, try again
                    }
                    if (errno == EPIPE) {
                        // FFmpeg closed the pipe (probably exited or crashed)
                        printf("FFmpeg closed the pipe - playback may have ended.\n");
                        close(pipe_write_fd);
                        pipe_write_fd = -1;
                        break;
                    }
                    perror("write to pipe");
                    close(pipe_write_fd);
                    pipe_write_fd = -1;
                    break;
                }
                if (w == 0) {
                    // Shouldn't happen with blocking write, but handle it
                    break;
                }
                sent += w;
            }
        }
    }

    // Cleanup: close pipe and file
    if (pipe_write_fd >= 0) {
        close(pipe_write_fd);
    }
    if (fp) {
        fclose(fp);
    }

    // Restore terminal to normal mode
    restore_terminal(&old_term, term_modified);
    
    if (abort_sent) {
        printf("Stream aborted by user. Returning to menu.\n");
        // Don't wait for FFmpeg - it's already been killed
        return 0;
    }
    
    // Wait for player to finish (if it hasn't already)
    wait_for_player_completion(player_pid);

    // Check if all data was received successfully
    if (remaining == 0) {
        printf("Stream completed successfully.\n");
        return 0;
    }
    return -1;  // Error: didn't receive all data
}

// Video-only streaming path using mpv player (alternative to ffplay).
// This function provides enhanced video playback with pause/resume controls
// and better handling of mpv's terminal interaction.
// Audio files continue to use the ffplay path above.
// Returns: 0 on success, -1 on error
static int stream_or_download_media_video_mpv(int sockfd, long long size, const char *title_hint) {
    int is_video = is_video_file(title_hint);
    if (!is_video) {
        // Should not be called for non-video; fall back defensively
        return stream_or_download_media(sockfd, size, title_hint);
    }

    FILE *fp = NULL;
    if (title_hint && *title_hint) {
        fp = fopen(title_hint, "wb");
        if (!fp) {
            perror("fopen");
            return -1;
        }
    } else {
        fprintf(stderr, "Error: No filename provided.\n");
        return -1;
    }

    int pipefd[2];
    int pipe_write_fd = -1;
    pid_t player_pid = -1;
    int streaming_enabled = 0;

    // Pipe-based streaming into mpv
    if (pipe(pipefd) == 0) {
        int pipe_size = 2 * 1024 * 1024;  // 2MB for video
        if (fcntl(pipefd[0], F_SETPIPE_SZ, pipe_size) < 0) {
            // Ignore failure, use default
        }
        if (fcntl(pipefd[1], F_SETPIPE_SZ, pipe_size) < 0) {
            // Ignore failure, use default
        }

        pipe_write_fd = pipefd[1];

        player_pid = launch_mpv_for_video(title_hint, pipefd[0]);
        if (player_pid > 0) {
            streaming_enabled = 1;
            printf("Streaming video via mpv/SDL (ultra-low-latency mode)...\n");
        } else {
            close(pipefd[1]);
            pipe_write_fd = -1;
        }
    } else {
        perror("pipe");
        streaming_enabled = 0;
    }

    long long remaining = size;
    char buffer[BUF_SIZE];

    // If streaming failed, just download the video
    if (!streaming_enabled) {
        printf("Streaming unavailable. Downloading video...\n");
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
            ssize_t r = recv(sockfd, buffer, chunk, 0);
            if (r <= 0) {
                if (r < 0) perror("recv");
                else printf("Server closed connection unexpectedly.\n");
                fclose(fp);
                return -1;
            }
            fwrite(buffer, 1, (size_t)r, fp);
            remaining -= r;
        }
        fclose(fp);
        printf("Download complete. Saved as '%s'.\n", title_hint);
        return 0;
    }

    struct termios old_term;
    memset(&old_term, 0, sizeof(old_term));
    int term_modified = enable_raw_terminal(&old_term);

    // Try to open /dev/tty for input so mpv can't steal it
    // /dev/tty is a direct reference to the controlling terminal, which helps
    // prevent mpv from interfering with keyboard input handling
    int tty_fd = -1;
    if (term_modified) {
        tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
        if (tty_fd < 0) {
            // Fallback to STDIN if /dev/tty fails (e.g., not a terminal)
            tty_fd = STDIN_FILENO;
            int flags = fcntl(tty_fd, F_GETFL);
            if (flags >= 0) {
                fcntl(tty_fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
        printf("Controls: SPACE = pause/unpause | E = exit streaming and return to menu\n");
        fflush(stdout);
    }

    // Playback control state
    int playback_active = 1;
    int abort_sent = 0;
    int input_check_counter = 0;  // Counter for periodic terminal mode re-application
    int paused = 0;                // Track pause state for toggle

    // Main streaming loop with enhanced controls for mpv
    while (remaining > 0 && playback_active) {
        // Periodically ensure terminal stays in raw mode
        // mpv may change terminal settings, so we re-apply raw mode every 10 iterations
        if (term_modified && (input_check_counter % 10 == 0)) {
            ensure_raw_terminal(&old_term);
        }
        input_check_counter++;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        int maxfd = sockfd;

        if (term_modified && tty_fd >= 0) {
            FD_SET(tty_fd, &rfds);
            if (tty_fd > maxfd) {
                maxfd = tty_fd;
            }
        }

        // Shorter timeout (20ms) for more responsive controls with mpv
        struct timeval tv = {0, 20000}; // 20ms
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        // Detect if mpv window was closed directly by user
        // kill(pid, 0) checks if process exists without sending a signal
        if (player_pid > 0 && playback_active && !abort_sent) {
            if (kill(player_pid, 0) != 0) {
                if (errno == ESRCH) {
                    // Process doesn't exist (window was closed)
                    printf("\nPlayer window was closed. Stopping streaming and returning to menu...\n");
                    fflush(stdout);
                    playback_active = 0;
                    abort_sent = 1;

                    if (pipe_write_fd >= 0) {
                        close(pipe_write_fd);
                        pipe_write_fd = -1;
                    }

                    if (send(sockfd, "ABORT\n", 6, 0) < 0) {
                        perror("send abort");
                    }

                    break;
                }
            }
        }

        // Keyboard controls for video playback
        if (term_modified && tty_fd >= 0 && FD_ISSET(tty_fd, &rfds)) {
            ensure_raw_terminal(&old_term);  // Re-apply raw mode (mpv may have changed it)

            // Read all available characters (non-blocking)
            while (1) {
                char ch;
                ssize_t r = read(tty_fd, &ch, 1);
                if (r <= 0) {
                    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("read from terminal");
                    }
                    break;  // No more input available
                }

                if (ch == ' ') {
                    // SPACE: Toggle pause/resume using SIGSTOP/SIGCONT
                    if (player_pid > 0 && !abort_sent) {
                        int sig = paused ? SIGCONT : SIGSTOP;  // Resume if paused, pause if playing
                        if (kill(player_pid, sig) != 0) {
                            if (errno != ESRCH) {
                                perror(paused ? "kill SIGCONT (resume)" : "kill SIGSTOP (pause)");
                            }
                        } else {
                            paused = !paused;  // Toggle pause state
                        }
                    }
                    ensure_raw_terminal(&old_term);
                    continue;
                } else if (ch == 'e' || ch == 'E') {
                    // E: Exit streaming and return to menu
                    if (playback_active && !abort_sent) {
                        printf("\nStopping streaming and returning to menu...\n");
                        fflush(stdout);
                        playback_active = 0;
                        abort_sent = 1;

                        if (pipe_write_fd >= 0) {
                            close(pipe_write_fd);
                            pipe_write_fd = -1;
                        }

                        if (send(sockfd, "ABORT\n", 6, 0) < 0) {
                            perror("send abort");
                        }

                        // Terminate mpv process (gentle then forceful)
                        if (player_pid > 0) {
                            if (kill(player_pid, SIGTERM) != 0 && errno != ESRCH) {
                                perror("kill SIGTERM");
                            } else {
                                usleep(100000); // 100ms wait for graceful exit
                                if (kill(player_pid, 0) == 0) {
                                    // Still running, force kill
                                    if (kill(player_pid, SIGKILL) != 0 && errno != ESRCH) {
                                        perror("kill SIGKILL");
                                    }
                                }
                            }
                        }
                        break;
                    }
                    break;
                }
            }
        }

        if (!FD_ISSET(sockfd, &rfds)) {
            continue;
        }

        size_t chunk = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
        ssize_t r = recv(sockfd, buffer, chunk, 0);
        if (r < 0) {
            perror("recv");
            break;
        }
        if (r == 0) {
            printf("Server closed connection unexpectedly during stream.\n");
            break;
        }

        remaining -= r;

        if (fp) {
            size_t written = fwrite(buffer, 1, (size_t)r, fp);
            if (written < (size_t)r && ferror(fp)) {
                perror("fwrite");
                fclose(fp);
                fp = NULL;
            }
        }

        // Write to pipe for streaming (only if not paused)
        // When paused, we stop writing to the pipe but continue receiving from server
        // and caching to file, so playback can resume seamlessly
        if (pipe_write_fd >= 0 && !paused) {
            ssize_t sent = 0;
            while (sent < r) {
                ssize_t w = write(pipe_write_fd, buffer + sent, (size_t)(r - sent));
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;  // Interrupted by signal, try again
                    }
                    if (errno == EPIPE) {
                        // Pipe broken - player closed or crashed
                        if (playback_active && !abort_sent) {
                            printf("\nPlayer closed. Stopping streaming and returning to menu...\n");
                            fflush(stdout);
                            playback_active = 0;
                            abort_sent = 1;

                            close(pipe_write_fd);
                            pipe_write_fd = -1;

                            if (send(sockfd, "ABORT\n", 6, 0) < 0) {
                                perror("send abort");
                            }

                            break;
                        } else {
                            close(pipe_write_fd);
                            pipe_write_fd = -1;
                            break;
                        }
                    }
                    perror("write to pipe");
                    close(pipe_write_fd);
                    pipe_write_fd = -1;
                    break;
                }
                if (w == 0) {
                    break;  // Shouldn't happen with blocking write
                }
                sent += w;
            }
        }
    }

    if (pipe_write_fd >= 0) {
        close(pipe_write_fd);
    }
    if (fp) {
        fclose(fp);
    }

    if (term_modified && tty_fd >= 0 && tty_fd != STDIN_FILENO) {
        close(tty_fd);
    }

    restore_terminal(&old_term, term_modified);

    if (abort_sent) {
        // Stream was stopped by user or window close; client stays connected
        return 0;
    }

    wait_for_player_completion(player_pid);

    if (remaining == 0) {
        printf("Stream completed successfully.\n");
        return 0;
    }
    return -1;
}

// Receives and displays the menu from the server.
// The server sends menu lines terminated by "END\n" to indicate completion.
// prefill: optional first line to display (used when server sends menu start in response)
// Returns: 0 on success, -1 on error
static int display_menu(int sockfd, const char *prefill) {
    char buffer[BUF_SIZE];

    // Display prefill line if provided (usually the first menu line)
    if (prefill) {
        printf("%s", prefill);
        if (strcmp(prefill, "END\n") == 0) {
            return 0;  // Menu already complete
        }
    }

    // Receive and display menu lines until "END\n" is received
    while (1) {
        ssize_t n = recv_line(sockfd, buffer, sizeof(buffer));
        if (n < 0) {
            printf("Error receiving menu.\n");
            return -1;
        }
        if (n == 0) {
            printf("Server closed connection.\n");
            return -1;
        }
        printf("%s", buffer);
        if (strcmp(buffer, "END\n") == 0) {
            break;  // Menu complete
        }
    }
    return 0;
}

// Main entry point: connects to server and runs interactive menu loop.
// The client maintains a persistent connection and can play multiple files
// in a single session. Type "exit" to quit.
int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUF_SIZE];
    int menu_cached = 0;  // Flag to avoid re-displaying menu unnecessarily

    // Ignore SIGPIPE signal (broken pipe) - we handle EPIPE errors explicitly
    // This prevents the program from crashing if the server closes the connection
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    // Validate command line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        fprintf(stderr, "Example: %s 192.168.10.10\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];

    // Create TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set up server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server %s:%d\n", server_ip, PORT);

    // Main interactive loop: display menu, get user choice, stream media
    while (1) {
        // Display menu if not cached (avoids re-displaying after streaming)
        if (!menu_cached) {
            printf("\n");
            if (display_menu(sockfd, NULL) < 0) {
                break;  // Error or connection closed
            }
            printf("\n");
            menu_cached = 1;
        }

        // Get user selection
        printf("Enter selection (number, 'back', or 'exit'): ");
        fflush(stdout);

        char choice_line[64];
        if (!fgets(choice_line, sizeof(choice_line), stdin)) {
            fprintf(stderr, "Error reading input.\n");
            break;
        }
        // Remove newline/carriage return
        choice_line[strcspn(choice_line, "\r\n")] = '\0';

        // Handle exit command
        if (strcmp(choice_line, "exit") == 0 || strcmp(choice_line, "EXIT") == 0 ||
            strcmp(choice_line, "0") == 0) {
            char send_line[128];
            snprintf(send_line, sizeof(send_line), "%s\n", choice_line);
            send(sockfd, send_line, strlen(send_line), 0);
            printf("Exiting client.\n");
            break;
        }

        // Handle back command (go to parent menu)
        if (strcasecmp(choice_line, "back") == 0 || strcasecmp(choice_line, "b") == 0) {
            char send_line[128];
            snprintf(send_line, sizeof(send_line), "%s\n", choice_line);
            send(sockfd, send_line, strlen(send_line), 0);
            menu_cached = 0;  // Force menu refresh
            continue;
        }

        // Send selection to server
        char send_line[128];
        snprintf(send_line, sizeof(send_line), "%s\n", choice_line);
        if (send(sockfd, send_line, strlen(send_line), 0) < 0) {
            perror("send");
            break;
        }

        // Receive server response
        ssize_t n = recv_line(sockfd, buffer, sizeof(buffer));
        if (n <= 0) {
            if (n == 0) {
                printf("Server closed connection.\n");
            } else {
                printf("Failed to read server response.\n");
            }
            break;
        }

        // Check if response is a menu (not a file stream)
        if (strncmp(buffer, "SIZE ", 5) != 0) {
            // Display new menu (buffer contains first line)
            if (display_menu(sockfd, buffer) < 0) {
                break;
            }
            printf("\n");
            menu_cached = 1;
            continue;  // Loop back to get next selection
        }

        // Response is "SIZE <size> [filename]" - start streaming
        long long size = 0;
        char remote_name[256] = "stream.bin";
        int parsed = sscanf(buffer, "SIZE %lld %255s", &size, remote_name);
        if (parsed < 1 || size <= 0) {
            printf("Unexpected header from server: %s\n", buffer);
            menu_cached = 0;
            continue;
        }
        // If no filename provided, generate one based on size
        if (parsed == 1) {
            snprintf(remote_name, sizeof(remote_name), "stream_%lld.bin", size);
        }

        printf("Streaming %lld bytes as '%s'...\n", size, remote_name);
        // Route to appropriate streaming function based on file type
        if (is_video_file(remote_name)) {
            // Use mpv for video files (better controls and performance)
            if (stream_or_download_media_video_mpv(sockfd, size, remote_name) < 0) {
                printf("Streaming failed. Closing client.\n");
                break;
            }
        } else {
            // Use ffplay for audio files
            if (stream_or_download_media(sockfd, size, remote_name) < 0) {
                printf("Streaming failed. Closing client.\n");
                break;
            }
        }
        menu_cached = 0;  // Refresh menu after streaming completes
    }

    // Cleanup: close socket connection
    close(sockfd);
    return 0;
}
