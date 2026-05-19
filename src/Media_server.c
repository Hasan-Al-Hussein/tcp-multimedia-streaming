Media_server.c (server c-code):
// media_server.c - TCP multimedia server that stays open
// - Sends a menu
// - Accepts choices in a loop from one client
// - For each choice, sends SIZE <bytes>\n + file data
// - Stops talking to that client only when it receives "exit" or "0"
// - Server itself keeps running forever, accepting new clients

// Standard I/O and utility libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// Network socket programming libraries
#include <arpa/inet.h>
// Multi-threading support for handling multiple clients concurrently
#include <pthread.h>
// File system operations (for getting file sizes)
#include <sys/stat.h>
// Select system call for checking incoming data during transfer
#include <sys/select.h>
// Character classification functions
#include <ctype.h>
// String comparison functions (case-insensitive)
#include <strings.h>

// Network configuration constants
#define PORT 9000                    // TCP port number the server listens on
#define BACKLOG 5                    // Maximum number of pending connections in queue
#define BUF_SIZE 4096                // Buffer size for reading/writing data (4KB chunks)
#define STREAM_THROTTLE_DELAY_US 20000  // 20 ms between chunks (~200 KB/s) - throttles transfer speed

// ANSI color codes for terminal output formatting
#define ANSI_RESET "\033[0m"         // Reset all formatting
#define ANSI_BANNER "\033[1;36m"     // Bold cyan for banner
#define ANSI_TITLE "\033[1;35m"      // Bold magenta for title
#define ANSI_TEXT "\033[0;37m"       // White text
#define ANSI_HIGHLIGHT "\033[38;5;213m"  // Pink highlight color

// Enumeration for menu navigation states
typedef enum {
    MENU_STAGE_CATEGORY = 0,  // User is selecting a category (Audio, Video, Image, File)
    MENU_STAGE_FILE = 1       // User is viewing files within a selected category
} menu_stage_t;

// Structure to track the current menu state for a client session
typedef struct {
    menu_stage_t stage;              // Current menu stage (category or file selection)
    int active_category;             // Index into CATEGORY_DEFS, -1 when none selected
} menu_state_t;

// Structure defining a media category with its properties
typedef struct {
    const char *label;               // Display name for the category
    const char *type_key;            // Matches detect_media_type output (e.g., "Audio", "Video")
    const char *subtext;             // Descriptive subtitle for the category
} category_descriptor_t;

// Array of category definitions - defines the available media categories
static const category_descriptor_t CATEGORY_DEFS[] = {
    {"Audio Lounge", "Audio", "Music, podcasts, playlists"},
    {"Video Theater", "Video", "Movies, lectures, highlights"}
};
// Calculate number of categories at compile time
static const int NUM_CATEGORY_DEFS = sizeof(CATEGORY_DEFS) / sizeof(CATEGORY_DEFS[0]);

// Update these paths to where your media files are on PC1
// Array of file paths that the server will serve to clients
static const char *media_files[] = {
    "/home/dslab/Desktop/UAE National Anthem.mp3",
    "/home/dslab/Desktop/Iraqi Song.mp3",
    "/home/dslab/Desktop/Messi Goal.mp4",
    "/home/dslab/Desktop/Rick Roll.mp4",
    "/home/dslab/Desktop/Never Gonna Give You Up.wav",
    "/home/dslab/Desktop/Ankara Messi.avi"
};
// Calculate number of files at compile time
static const int NUM_FILES = sizeof(media_files) / sizeof(media_files[0]);

// Extract the filename (basename) from a full file path
// Handles both Unix-style (/) and Windows-style (\) path separators
static const char *basename_from_path(const char *path) {
    if (!path) {
        return "<unknown>";
    }
    // Find the last forward slash
    const char *slash = strrchr(path, '/');
    // Find the last backslash (for Windows paths)
    const char *backslash = strrchr(path, '\\');
    // Determine which separator comes last
    const char *last_sep = slash;
    if (backslash && (!last_sep || backslash > last_sep)) {
        last_sep = backslash;
    }
    // Return pointer to character after last separator, or the whole path if no separator
    return last_sep ? last_sep + 1 : path;
}

// Sanitize a filename by replacing unsafe characters with underscores
// Used to create safe labels for file transmission headers
static void make_safe_label(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0) {
        return;
    }
    // Default name if source is NULL
    if (!src) {
        src = "stream";
    }
    size_t j = 0;
    // Iterate through source string and sanitize each character
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        // Replace control characters and filesystem-unsafe characters with underscore
        if (iscntrl(c) || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            dst[j++] = '_';
        } else if (isspace(c)) {
            // Replace whitespace with underscore
            dst[j++] = '_';
        } else {
            // Keep safe characters as-is
            dst[j++] = (char)c;
        }
    }
    // Ensure at least one character in output
    if (j == 0) {
        dst[j++] = 's';
    }
    dst[j] = '\0';
}

// Check if a filename has a specific extension (case-insensitive)
// Returns 1 if the extension matches, 0 otherwise
static int has_extension(const char *filename, const char *ext) {
    if (!filename || !ext) {
        return 0;
    }
    // Find the last dot in the filename
    const char *dot = strrchr(filename, '.');
    if (!dot || *(dot + 1) == '\0') {
        return 0;  // No extension found
    }
    dot++;  // Move past the dot
    // Compare extension character by character (case-insensitive)
    while (*dot && *ext) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext)) {
            return 0;  // Mismatch found
        }
        dot++;
        ext++;
    }
    // Both strings must end at the same time for a match
    return *dot == '\0' && *ext == '\0';
}

// Detect the media type of a file based on its extension
// Returns "Audio", "Video", "Image", or "File" (default)
static const char *detect_media_type(const char *filename) {
    if (!filename) {
        return "File";
    }
    // Check for audio file extensions
    if (has_extension(filename, "mp3") || has_extension(filename, "wav") ||
        has_extension(filename, "aac") || has_extension(filename, "flac")) {
        return "Audio";
    }
    // Check for video file extensions
    if (has_extension(filename, "mp4") || has_extension(filename, "mkv") ||
        has_extension(filename, "mov") || has_extension(filename, "avi")) {
        return "Video";
    }
    // Check for image file extensions
    if (has_extension(filename, "jpg") || has_extension(filename, "jpeg") ||
        has_extension(filename, "png") || has_extension(filename, "gif")) {
        return "Image";
    }
    // Default to generic "File" type
    return "File";
}

// Check if a file belongs to a specific category
// Returns 1 if the file's detected type matches the category's type_key, 0 otherwise
static int matches_category(int file_index, int category_index) {
    // Validate category index
    if (category_index < 0 || category_index >= NUM_CATEGORY_DEFS) {
        return 0;
    }
    // Validate file index
    if (file_index < 0 || file_index >= NUM_FILES) {
        return 0;
    }
    // Get the filename and detect its media type
    const char *title = basename_from_path(media_files[file_index]);
    const char *type = detect_media_type(title);
    // Compare detected type with category's expected type
    return strcmp(type, CATEGORY_DEFS[category_index].type_key) == 0;
}

// Count how many files belong to a specific category
// Returns the total count of files matching the category
static int count_items_in_category(int category_index) {
    int count = 0;
    // Iterate through all files and count matches
    for (int i = 0; i < NUM_FILES; i++) {
        if (matches_category(i, category_index)) {
            count++;
        }
    }
    return count;
}

// Get the file index for the nth file in a category (1-based selection)
// Returns the index in media_files[] array, or -1 if selection is invalid
static int get_nth_file_index_in_category(int category_index, int selection) {
    if (selection <= 0) {
        return -1;  // Invalid selection number
    }
    // Iterate through files, counting only those in the specified category
    for (int i = 0; i < NUM_FILES; i++) {
        if (matches_category(i, category_index)) {
            selection--;  // Decrement selection counter
            if (selection == 0) {
                return i;  // Found the nth file in this category
            }
        }
    }
    return -1;  // Selection number exceeds available files in category
}

// Get ANSI color code for a media type (for terminal display)
// Returns appropriate color escape sequence based on media type
static const char *color_for_media_type(const char *type) {
    if (!type) {
        return ANSI_RESET;
    }
    if (strcmp(type, "Audio") == 0) {
        return "\033[38;5;81m";   // Cyan for audio
    }
    if (strcmp(type, "Video") == 0) {
        return "\033[38;5;207m";  // Pink for video
    }
    if (strcmp(type, "Image") == 0) {
        return "\033[38;5;190m";  // Yellow for images
    }
    return "\033[1;37m";  // Bold white for generic files
}

// Get the size of a file in bytes using stat()
// Returns 0 on success, -1 on error (file not found or invalid parameters)
static int get_file_size_bytes(const char *path, long long *out_bytes) {
    if (!path || !out_bytes) {
        return -1;
    }
    struct stat st;
    // Get file statistics
    if (stat(path, &st) != 0) {
        return -1;  // File doesn't exist or can't be accessed
    }
    // Store file size in output parameter
    *out_bytes = (long long)st.st_size;
    return 0;
}

// Format a byte count into a human-readable string (B, KB, MB, GB, TB)
// Converts bytes to appropriate unit and formats with proper precision
static void format_size_string(long long bytes, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    // Convert to double, handling negative values
    double size = (bytes < 0) ? 0.0 : (double)bytes;
    int unit_index = 0;
    // Convert to appropriate unit (divide by 1024 until size < 1024)
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    // Format with no decimal places for large numbers, one decimal for smaller
    if (size >= 100.0) {
        snprintf(out, cap, "%4.0f %s", size, units[unit_index]);
    } else {
        snprintf(out, cap, "%4.1f %s", size, units[unit_index]);
    }
}

// Thread-safe client ID generation
static pthread_mutex_t client_id_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for protecting next_client_id
static int next_client_id = 1;  // Counter for assigning unique IDs to each client

// Send all data in a buffer over a socket, handling partial sends
// Ensures the entire buffer is sent even if send() doesn't send everything at once
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    // Keep sending until all bytes are transmitted
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            perror("send");
            return -1;  // Error occurred
        }
        // Advance pointer and decrease remaining length
        p += n;
        len -= n;
    }
    return 0;  // Success
}

// Receive one line ending with '\n' from a socket
// Reads character by character until newline or buffer full
// Returns number of bytes read, 0 on connection close, -1 on error
static ssize_t recv_line(int fd, char *buf, size_t cap) {
    size_t i = 0;
    // Read one character at a time until newline or buffer full
    while (i + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            perror("recv");
            return -1;  // Error receiving
        }
        if (n == 0) {
            // Connection closed by peer
            if (i == 0) return 0;  // No data received before close
            break;  // Some data received, return it
        }
        buf[i++] = c;
        if (c == '\n') break;  // End of line found
    }
    buf[i] = '\0';  // Null-terminate the string
    return (ssize_t)i;
}

// Send the menu to the client based on current menu state
// Displays either category selection or file list within a category
// Returns 0 on success, -1 on error
static int send_menu(int client_fd, menu_state_t *menu_state) {
    if (!menu_state) {
        return -1;
    }

    char line[BUF_SIZE];  // Buffer for formatting menu lines

    // Structure to hold file information for menu display
    typedef struct {
        const char *title;      // Filename to display
        const char *type;       // Media type (Audio, Video, etc.)
        const char *color;      // ANSI color code for this type
        char size_label[16];    // Formatted size string (e.g., "1.5 MB")
        long long bytes;        // File size in bytes
        int has_size;           // 1 if size was successfully retrieved, 0 otherwise
    } file_row_t;

    // Array to store information about each file for menu display
    file_row_t rows[NUM_FILES];
    long long total_bytes = 0;  // Sum of all file sizes
    int sized_entries = 0;      // Count of files with valid size information
    // Arrays to track category statistics
    long long category_bytes[NUM_CATEGORY_DEFS];  // Total bytes per category
    int category_counts[NUM_CATEGORY_DEFS];       // File count per category
    memset(category_bytes, 0, sizeof(category_bytes));
    memset(category_counts, 0, sizeof(category_counts));

    // Process each file to gather information for menu display
    for (int i = 0; i < NUM_FILES; i++) {
        // Extract filename and detect media type
        rows[i].title = basename_from_path(media_files[i]);
        rows[i].type = detect_media_type(rows[i].title);
        rows[i].color = color_for_media_type(rows[i].type);
        
        // Try to get file size
        long long bytes = 0;
        if (get_file_size_bytes(media_files[i], &bytes) == 0) {
            rows[i].bytes = bytes;
            rows[i].has_size = 1;
            format_size_string(bytes, rows[i].size_label, sizeof(rows[i].size_label));
            total_bytes += bytes;  // Add to total library size
            sized_entries++;
        } else {
            // File size unavailable (file doesn't exist or can't be accessed)
            rows[i].bytes = -1;
            rows[i].has_size = 0;
            snprintf(rows[i].size_label, sizeof(rows[i].size_label), "   n/a");
        }

        // Update category statistics
        for (int cat = 0; cat < NUM_CATEGORY_DEFS; cat++) {
            if (strcmp(rows[i].type, CATEGORY_DEFS[cat].type_key) == 0) {
                category_counts[cat]++;  // Increment file count for this category
                if (rows[i].has_size) {
                    category_bytes[cat] += rows[i].bytes;  // Add to category total size
                }
                break;
            }
        }
    }

    // Format total library size for display
    char total_size_label[16];
    if (sized_entries > 0) {
        format_size_string(total_bytes, total_size_label, sizeof(total_size_label));
    } else {
        snprintf(total_size_label, sizeof(total_size_label), "unknown");
    }

    // Validate menu state - if viewing files but category is invalid/empty, reset to category view
    if (menu_state->stage == MENU_STAGE_FILE) {
        int idx = menu_state->active_category;
        if (idx < 0 || idx >= NUM_CATEGORY_DEFS || category_counts[idx] == 0) {
            // Invalid or empty category, reset to category selection
            menu_state->stage = MENU_STAGE_CATEGORY;
            menu_state->active_category = -1;
        }
    }

    // Clear the client's terminal screen
    const char *clear_screen = "\033[2J\033[H";
    if (send_all(client_fd, clear_screen, strlen(clear_screen)) < 0) return -1;

    // Send banner header
    const char *banner =
        ANSI_BANNER "+======================================================================+\n" ANSI_RESET
        ANSI_TITLE "|                   MEDIA STREAM EXPERIENCE HUB                       |\n" ANSI_RESET
        ANSI_BANNER "+======================================================================+\n" ANSI_RESET;
    if (send_all(client_fd, banner, strlen(banner)) < 0) return -1;

    // Send tagline
    const char *tagline =
        ANSI_TEXT "   curate | stream | relax\n\n" ANSI_RESET;
    if (send_all(client_fd, tagline, strlen(tagline)) < 0) return -1;

    // Send library statistics
    snprintf(line, sizeof(line),
             "%s   Library: %d titles   |   Storage footprint: %s%s\n",
             ANSI_HIGHLIGHT, NUM_FILES, total_size_label, ANSI_RESET);
    if (send_all(client_fd, line, strlen(line)) < 0) return -1;

    // Send color legend explaining media type colors
    const char *legend =
        "   Legend: "
        "\033[38;5;81mAudio\033[0m  "
        "\033[38;5;207mVideo\033[0m  "
        "\033[38;5;190mImage\033[0m  "
        "\033[1;37mFile\033[0m\n\n";
    if (send_all(client_fd, legend, strlen(legend)) < 0) return -1;

    // Display category selection menu
    if (menu_state->stage == MENU_STAGE_CATEGORY) {
        // Send table header for category list
        const char *table_header =
            "\033[0;36m+----+----------------------+--------------------------+------------+\n"
            "| ID | Category             | Description              | Items/Size |\n"
            "+----+----------------------+--------------------------+------------+\033[0m\n";
        if (send_all(client_fd, table_header, strlen(table_header)) < 0) return -1;

        // Send each category as a table row
        for (int i = 0; i < NUM_CATEGORY_DEFS; i++) {
            char size_buf[16];
            // Format category total size if available
            if (category_counts[i] > 0 && category_bytes[i] > 0) {
                format_size_string(category_bytes[i], size_buf, sizeof(size_buf));
            } else {
                snprintf(size_buf, sizeof(size_buf), "--");
            }

            // Format and send category row
            snprintf(line, sizeof(line),
                     "| %2d | %-20.20s | %-24.24s | %2d / %-6s |\n",
                     i + 1,  // Category ID (1-based)
                     CATEGORY_DEFS[i].label,
                     CATEGORY_DEFS[i].subtext,
                     category_counts[i],  // Number of files in category
                     size_buf);  // Total size of files in category
            if (send_all(client_fd, line, strlen(line)) < 0) return -1;
        }

        // Send table footer
        const char *table_footer =
            "\033[0;36m+----+----------------------+--------------------------+------------+\033[0m\n";
        if (send_all(client_fd, table_footer, strlen(table_footer)) < 0) return -1;

        // Send usage instructions
        const char *tips =
            "\nInstructions:\n"
            "  - Enter the category ID you want to explore.\n"
            "  - Enter 0 (or type 'exit') whenever you want to end the session.\n";
        if (send_all(client_fd, tips, strlen(tips)) < 0) return -1;
    } else {
        // Display file list within selected category
        int cat_idx = menu_state->active_category;
        if (cat_idx < 0 || cat_idx >= NUM_CATEGORY_DEFS) {
            return -1;  // Invalid category index
        }

        // Send category header showing which category is being browsed
        snprintf(line, sizeof(line),
                 "\n%s   Browsing: %s  |  %d items available%s\n",
                 ANSI_HIGHLIGHT,
                 CATEGORY_DEFS[cat_idx].label,
                 category_counts[cat_idx],
                 ANSI_RESET);
        if (send_all(client_fd, line, strlen(line)) < 0) return -1;

        // Send table header for file list
        const char *table_header =
            "\033[0;36m+----+--------------------------------+---------+------------+\n"
            "| ID | Title                          | Type    | Size       |\n"
            "+----+--------------------------------+---------+------------+\033[0m\n";
        if (send_all(client_fd, table_header, strlen(table_header)) < 0) return -1;

        // Display each file in the category
        int visible_index = 0;  // 1-based index for display (starts at 1)
        for (int i = 0; i < NUM_FILES; i++) {
            // Skip files that don't belong to this category
            if (!matches_category(i, cat_idx)) {
                continue;
            }

            // Format and send file row with color coding
            snprintf(line, sizeof(line),
                     "%s| %2d | %-30.30s | %-7s | %10s |%s\n",
                     rows[i].color,        // Color based on media type
                     visible_index + 1,    // Display ID (1-based)
                     rows[i].title,        // Filename
                     rows[i].type,         // Media type
                     rows[i].size_label,   // Formatted size
                     ANSI_RESET);
            if (send_all(client_fd, line, strlen(line)) < 0) return -1;
            visible_index++;
        }

        // Send table footer
        const char *table_footer =
            "\033[0;36m+----+--------------------------------+---------+------------+\033[0m\n";
        if (send_all(client_fd, table_footer, strlen(table_footer)) < 0) return -1;

        // Send usage instructions for file selection
        const char *tips =
            "\nControls:\n"
            "  - Enter the listed ID to download & play that media immediately.\n"
            "  - Type 'back' (or 'b') to return to the category selection.\n"
            "  - Enter 0 (or type 'exit') whenever you want to end the session.\n";
        if (send_all(client_fd, tips, strlen(tips)) < 0) return -1;
    }

    // Send footer banner
    const char *footer =
        "\n" ANSI_BANNER "+======================================================================+\n" ANSI_RESET "\n";
    if (send_all(client_fd, footer, strlen(footer)) < 0) return -1;

    // Send END marker to signal menu transmission is complete
    if (send_all(client_fd, "END\n", 4) < 0) return -1;

    return 0;
}

// Main client handling function - manages the interaction loop with a single client
// Handles menu display, user input, and file transmission
static void handle_client(int client_fd, int client_id) {
    char line[BUF_SIZE];      // Buffer for receiving user input
    char buffer[BUF_SIZE];    // Buffer for reading file data
    // Initialize menu state to category selection stage
    menu_state_t menu_state = {MENU_STAGE_CATEGORY, -1};

    printf("Client #%d connected.\n", client_id);

    // Main interaction loop - continues until client disconnects or requests exit
    while (1) {
        // 1) Send menu each round (category or file list based on state)
        if (send_menu(client_fd, &menu_state) < 0) {
            printf("Error sending menu to client #%d, closing client.\n", client_id);
            break;
        }

        // 2) Receive user's choice/command
        ssize_t n = recv_line(client_fd, line, sizeof(line));
        if (n <= 0) {
            printf("Client #%d disconnected while waiting for choice.\n", client_id);
            break;
        }

        // Strip newline and carriage return characters
        line[strcspn(line, "\r\n")] = '\0';

        // Check for exit command (case-insensitive or "0")
        if (strcmp(line, "exit") == 0 || strcmp(line, "EXIT") == 0 ||
            strcmp(line, "0") == 0) {
            printf("Client #%d requested exit.\n", client_id);
            break;
        }

        // Handle category selection stage
        if (menu_state.stage == MENU_STAGE_CATEGORY) {
            int category_choice = atoi(line);  // Convert input to integer
            // Validate category selection (1-based input)
            if (category_choice < 1 || category_choice > NUM_CATEGORY_DEFS) {
                const char *msg = "Invalid category selection.\n";
                send_all(client_fd, msg, strlen(msg));
                continue;  // Loop back to send menu again
            }

            int category_index = category_choice - 1;  // Convert to 0-based index
            int available = count_items_in_category(category_index);
            // Check if category has any files
            if (available <= 0) {
                const char *msg = "That category is currently empty. Please choose another option.\n";
                send_all(client_fd, msg, strlen(msg));
                continue;
            }

            // Transition to file selection stage for this category
            menu_state.stage = MENU_STAGE_FILE;
            menu_state.active_category = category_index;
            continue;  // Loop back to send file list menu
        }

        // Handle "back" command while viewing files in a category
        if (strcasecmp(line, "back") == 0 || strcasecmp(line, "b") == 0) {
            // Return to category selection
            menu_state.stage = MENU_STAGE_CATEGORY;
            menu_state.active_category = -1;
            continue;  // Loop back to send category menu
        }

        // Handle file selection (user is in file list stage)
        int selection = atoi(line);  // Convert input to integer
        if (selection < 1) {
            const char *msg = "Invalid selection for this category.\n";
            send_all(client_fd, msg, strlen(msg));
            continue;
        }

        // Get the actual file index from the selection number
        int file_index = get_nth_file_index_in_category(menu_state.active_category, selection);
        if (file_index < 0) {
            const char *msg = "That ID is not available in this category.\n";
            send_all(client_fd, msg, strlen(msg));
            continue;
        }

        // Get the full file path
        const char *filename = media_files[file_index];
        printf("Client #%d selected: %s\n", client_id, basename_from_path(filename));

        // Open file in binary read mode
        FILE *fp = fopen(filename, "rb");
        if (!fp) {
            perror("fopen");
            const char *msg = "ERROR: could not open file on server.\n";
            send_all(client_fd, msg, strlen(msg));
            continue;  // Try again with menu
        }

        // Get file size by seeking to end
        if (fseek(fp, 0, SEEK_END) != 0) {
            perror("fseek");
            fclose(fp);
            continue;
        }
        long long size = ftell(fp);
        if (size < 0) {
            perror("ftell");
            fclose(fp);
            continue;
        }
        rewind(fp);  // Reset file pointer to beginning

        // 3) Send header: SIZE <bytes> <safe_name>\n
        // Client expects this format to know file size and name before receiving data
        char header[256];
        char safe_label[128];
        make_safe_label(basename_from_path(filename), safe_label, sizeof(safe_label));
        snprintf(header, sizeof(header), "SIZE %lld %s\n", size, safe_label);
        if (send_all(client_fd, header, strlen(header)) < 0) {
            fclose(fp);
            break;  // Connection error, exit loop
        }

        // 4) Send file content in chunks
        long long remaining = size;
        int transfer_aborted = 0;
        while (remaining > 0 && !transfer_aborted) {
            // Check for incoming ABORT command from client before sending next chunk
            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(client_fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 0;  // Non-blocking check
            
            int ready = select(client_fd + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET(client_fd, &rfds)) {
                // Data available - check if it's an ABORT command
                char abort_check[16];
                ssize_t n = recv_line(client_fd, abort_check, sizeof(abort_check));
                if (n > 0) {
                    // Strip newline
                    abort_check[strcspn(abort_check, "\r\n")] = '\0';
                    // Check if it's an ABORT command
                    if (strcmp(abort_check, "ABORT") == 0) {
                        printf("Client #%d requested to stop the streaming of '%s'.\n", client_id, basename_from_path(filename));
                        transfer_aborted = 1;
                        break;  // Exit transfer loop
                    }
                } else if (n == 0) {
                    // Connection closed
                    printf("Client #%d disconnected during transfer.\n", client_id);
                    fclose(fp);
                    close(client_fd);
                    return;
                }
            }
            
            // Determine how much to read (don't exceed buffer size)
            size_t to_read = (remaining < BUF_SIZE) ? (size_t)remaining : BUF_SIZE;
            size_t r = fread(buffer, 1, to_read, fp);
            if (r == 0) {
                if (ferror(fp)) {
                    perror("fread");
                }
                break;  // Error or EOF
            }
            // Send the chunk to client
            if (send_all(client_fd, buffer, r) < 0) {
                // Check if it's because client closed connection (EPIPE) or aborted
                // In that case, just break instead of closing connection
                printf("Client #%d stopped receiving data for '%s'.\n", client_id, basename_from_path(filename));
                transfer_aborted = 1;
                break;
            }
            remaining -= r;

            // Throttle transmission speed to avoid overwhelming network/client
            if (remaining > 0 && STREAM_THROTTLE_DELAY_US > 0) {
                usleep(STREAM_THROTTLE_DELAY_US);
            }
        }

        fclose(fp);
        if (!transfer_aborted) {
            printf("Finished sending '%s' to client #%d.\n", basename_from_path(filename), client_id);
        }
        // Then loop again, send menu again, wait for next choice
    }

    // Clean up: close client socket
    close(client_fd);
    printf("Client #%d disconnected.\n", client_id);
}

// Structure to pass client information to thread function
typedef struct {
    int client_fd;   // Socket file descriptor for client connection
    int client_id;   // Unique identifier for this client
} client_ctx_t;

// Thread function that handles a single client connection
// Each client gets its own thread so multiple clients can be served concurrently
static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    if (!ctx) return NULL;
    // Handle the client interaction (menu, file transfer, etc.)
    handle_client(ctx->client_fd, ctx->client_id);
    // Free the context structure allocated in main()
    free(ctx);
    return NULL;
}

// Main function - sets up TCP server and accepts client connections
int main(void) {
    int server_fd;              // Server socket file descriptor
    struct sockaddr_in addr;    // Server address structure

    // Create TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Enable SO_REUSEADDR to allow binding to port even if it was recently used
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Initialize address structure
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;           // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;   // Listen on all network interfaces
    addr.sin_port = htons(PORT);         // Convert port to network byte order

    // Bind socket to address and port
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Start listening for incoming connections
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Media server listening on port %d...\n", PORT);

    // Main server loop - accepts connections and spawns threads
    while (1) {
        struct sockaddr_in client_addr;  // Client address structure
        socklen_t client_len = sizeof(client_addr);
        // Accept incoming connection (blocks until client connects)
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;  // Try accepting next connection
        }

        // Generate unique client ID (thread-safe)
        int client_id;
        pthread_mutex_lock(&client_id_mutex);
        client_id = next_client_id++;
        pthread_mutex_unlock(&client_id_mutex);

        // Allocate context structure for thread
        client_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            perror("malloc");
            close(client_fd);
            continue;  // Skip this client if allocation fails
        }

        // Initialize context with client information
        ctx->client_fd = client_fd;
        ctx->client_id = client_id;

        // Log client connection with IP address and port
        char addr_str[INET_ADDRSTRLEN] = "<unknown>";
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        printf("Accepted client #%d from %s:%d\n", client_id, addr_str, ntohs(client_addr.sin_port));

        // Create thread to handle this client (allows concurrent clients)
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, ctx) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            continue;  // Skip this client if thread creation fails
        }
        // Detach thread so it cleans up automatically when done
        pthread_detach(thread);
    }

    // This line is never reached (infinite loop), but included for completeness
    close(server_fd);
}
