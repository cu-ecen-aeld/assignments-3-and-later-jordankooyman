/*
 * aesdsocket.c - Socket server that receives and stores data packets
 * Enhanced with thread management and timestamps (optional)
 *
 * Features:
 * - Listens on port 9000 for TCP connections
 * - Receives data until newline, appends to either /var/tmp/aesdsocketdata or /dev/aesdchar
 * - Returns entire file content after each complete packet
 * - Supports daemon mode with -d flag
 * - Supports multiple simultaneous socket connections with threads
 * - Graceful shutdown on SIGINT/SIGTERM (including all spawned threads)
 * - Includes timestamps written to output file every 10 seconds (disabled when using /dev/aesdchar)
 * - Build switch USE_AESD_CHAR_DEVICE (default 1) redirects I/O to the AESD character driver
 * - Supports AESDCHAR_IOCSEEKTO:X,Y socket command to seek via ioctl before reading
 *
 *  Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
 *  Version 2 Code (this): https://chat.deepseek.com/share/qtyyz0zhqx67gk3lir
 *   Comparison: https://chatgpt.com/share/697cea7e-2700-8007-8fa5-a7edea60a08d
 *
 * Refactored for Assignment 8 with the following improvements:
 *   - Fixed thread join deadlock by joining threads outside list mutex
 *   - Made signal handler async-signal-safe
 *   - Added partial write handling in file I/O
 *   - Avoid holding file mutex during network send (reads entire file into buffer under lock)
 *   - Verify /dev/aesdchar exists at startup when enabled
 *   - Added optional accept4() with CLOEXEC, accept error handling, and socket timeouts
 *
 *  Version 1 Update (this): https://chat.deepseek.com/share/zwsr2jih5tudk5thqa
 *  Version 2 Update: https://chat.deepseek.com/share/h6pac8gevqhneunhdq
 *   Comparison: https://chatgpt.com/share/69a0c8e6-0d4c-8007-a1c3-f982931753fe
 *
 * Revised for Assignment 9:
 *   - Added AESDCHAR_IOCSEEKTO:X,Y socket command support
 *   - Command is parsed, ioctl issued on same fd used for subsequent read (per spec)
 *   - Command string is never written to the device
 *   - Mutex held only around ioctl+read, released before network send
 *   - Fixed non-reentrant localtime() -> localtime_r() in timestamp thread
 *   - Added O_CLOEXEC to all thread-opened file descriptors
 *
 *  Version 1 Update: https://chat.deepseek.com/share/lfs1yxckkq0ibwh836
 *  Version 2 Update: https://chat.deepseek.com/share/nsei5ey2nu3sj77cwk
 *  Version 3 Update (used): https://claude.ai/share/940b08c1-937e-4f59-9fe4-db34cee65a86
 *   Comparison: https://claude.ai/share/c3549ecc-2b6d-4bd4-9e64-26b1f6f6e914
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>  /* UINT32_MAX */

/* ==================== Build Configuration ==================== */
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1   /* Default to using the character driver */
#endif

/* Select the data endpoint based on the build switch */
#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"           /* Character device */
#else
#define DATA_FILE "/var/tmp/aesdsocketdata" /* Regular file     */
#endif
/* ============================================================= */

/*
 * ==================== AESD ioctl Interface ====================
 * Fix 1: Prefer the project's shared aesd_ioctl.h header when building
 * against the driver tree.  Pass -DHAVE_AESD_IOCTL_H (and the appropriate
 * -I flag) to the compiler / Makefile to activate the real header.
 *
 * Example Makefile addition:
 *   CFLAGS += -I$(DRIVER_DIR) -DHAVE_AESD_IOCTL_H
 *
 * The inline fallback below is used only when the project header is absent
 * (e.g., building standalone for unit tests or on a host without the driver
 * tree).  The #ifndef guards prevent collisions if the real header is later
 * also included transitively.
 */
#ifdef HAVE_AESD_IOCTL_H
#  include "aesd_ioctl.h"
#else
#  ifndef AESD_IOC_MAGIC
#    define AESD_IOC_MAGIC  0x16
#  endif
   /*
    * struct aesd_seekto - ioctl argument for AESDCHAR_IOCSEEKTO.
    *
    * uint32_t is used instead of 'unsigned int' because it guarantees exactly
    * 32 bits on all platforms, matching the kernel driver struct.  'unsigned
    * int' is 32 bits on most Linux targets but the C standard does not require
    * it, so the types would silently diverge on a 64-bit-int platform.
    */
   struct aesd_seekto {
       uint32_t write_cmd;        /* Zero-based index of the write command to seek into */
       uint32_t write_cmd_offset; /* Zero-based byte offset within that write command   */
   };
#  ifndef AESDCHAR_IOCSEEKTO
     /*
      * _IOW(type, nr, datatype) encodes a 32-bit ioctl command number:
      *   type (bits 8-15)  : AESD_IOC_MAGIC (0x16) – driver cookie to reduce
      *                        collision risk with other drivers' ioctl numbers.
      *   nr   (bits 0-7)   : command index within this driver (1).
      *   size (bits 16-29) : sizeof(struct aesd_seekto), embedded by the macro
      *                        for kernel copyin/copyout size validation.
      *   direction (30-31) : _IOW = data flows user-space -> driver ("write").
      */
#    define AESDCHAR_IOCSEEKTO _IOWR(AESD_IOC_MAGIC, 1, struct aesd_seekto)
#  endif
#endif /* HAVE_AESD_IOCTL_H */

/*
 * SEEKTO_CMD_PREFIX - magic string prefix that identifies a seek command.
 *
 * Defined as a macro so strlen(SEEKTO_CMD_PREFIX) is always consistent with
 * the string itself.  Hard-coding the length (19 characters) would create a
 * maintenance hazard where the count could silently diverge from the string.
 */
#define SEEKTO_CMD_PREFIX "AESDCHAR_IOCSEEKTO:"
/* ============================================================= */

/* Configuration constants */
#define PORT 9000
#define RECV_BUFFER_SIZE 1024
#define MAX_PACKET_SIZE (10 * 1024 * 1024)
#define TIMESTAMP_INTERVAL 10 /* seconds (only used when !USE_AESD_CHAR_DEVICE) */
#define ACCEPT_RETRY_DELAY_MS 100  /* delay after accept() errors like EMFILE */

/* Thread node for linked list */
struct thread_node {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool active;
    struct thread_node *next;
};

/* Thread argument structure */
struct thread_args {
    int client_fd;
    struct sockaddr_in client_addr;
};

/* Global variables */
static volatile sig_atomic_t shutdown_requested = 0;
static int server_fd = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct thread_node *thread_list_head = NULL;

/* Timestamp thread related – only used when NOT using the character device */
#if !USE_AESD_CHAR_DEVICE
static pthread_t timestamp_thread;
static bool timestamp_thread_running = false;
static pthread_mutex_t timestamp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timestamp_cond = PTHREAD_COND_INITIALIZER;
#endif

static bool daemon_mode = false;

/* ---- Forward declarations ---- */
static void signal_handler(int signal);
static int setup_signal_handlers(void);
static int setup_server_socket(void);
static void cleanup_resources(void);
static void *connection_handler(void *arg);
static int run_as_daemon(void);
static void add_thread_to_list(pthread_t thread_id, int client_fd,
                               struct sockaddr_in *client_addr);
static void remove_thread_from_list(pthread_t thread_id);
static void wait_for_all_threads(void);

/*
 * Fix 6 / Fix 7: write_data_to_file and read_and_send_file are only compiled
 * when !USE_AESD_CHAR_DEVICE.  The char-device path has its own equivalents
 * (write_and_readback_chardev, handle_seekto_command) so the dead O_WRONLY /
 * O_APPEND branch that existed inside write_data_to_file is eliminated.
 * timestamp_thread_func is similarly file-path only.
 */
#if !USE_AESD_CHAR_DEVICE
static int write_data_to_file(const char *data, size_t length);
static int read_and_send_file(int client_fd);
static void *timestamp_thread_func(void *arg);
#endif /* !USE_AESD_CHAR_DEVICE */

/*
 * signal_handler - Handle SIGINT and SIGTERM signals.
 *
 * Async-signal-safe: only sets a volatile flag and shuts down the server
 * socket.  All other cleanup is performed in main() / cleanup_resources()
 * after the signal returns.
 */
static void signal_handler(int signal)
{
    (void)signal; /* parameter unused; the flag conveys the needed information */
    shutdown_requested = 1;

    /* Shutdown server socket to unblock accept() */
    if (server_fd != -1)
        shutdown(server_fd, SHUT_RDWR);
}

/*
 * setup_signal_handlers - Register handlers for SIGINT and SIGTERM.
 */
static int setup_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    /*
     * SA_RESTART is intentionally omitted.  With SA_RESTART, the kernel would
     * automatically restart accept() after a signal, causing the main loop to
     * block indefinitely even after a SIGINT.  Without it, accept() returns
     * EINTR, the loop checks shutdown_requested, and exits cleanly.
     */
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set SIGINT handler: %s", strerror(errno));
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set SIGTERM handler: %s", strerror(errno));
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/*
 * setup_server_socket - Create, configure, bind, and listen on the server socket.
 *
 * SOCK_CLOEXEC: atomically marks the fd close-on-exec so that child processes
 * created by run_as_daemon()'s double-fork do not inherit the listening socket.
 * Without this flag there is a race window between socket() and a hypothetical
 * fcntl(F_SETFD) call.
 */
static int setup_server_socket(void)
{
    int sock_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

#ifdef SOCK_CLOEXEC
    sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (sock_fd == -1) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
        syslog(LOG_WARNING, "Failed to set SO_REUSEADDR: %s", strerror(errno));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "Failed to bind to port %d: %s", PORT, strerror(errno));
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, SOMAXCONN) == -1) {
        syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * read_entire_file - Read an already-open fd from its current position to EOF
 * into a dynamically allocated heap buffer.
 *
 * Returns the buffer pointer (caller must free) and sets *out_size to the
 * number of bytes read.  Returns NULL on allocation or read error.
 *
 * This helper is shared between both the regular-file and char-device paths.
 * It is the caller's responsibility to hold file_mutex if the read must be
 * atomic with respect to concurrent writes.
 */
static char *read_entire_file(int fd, size_t *out_size)
{
    char *buffer = NULL;
    size_t capacity = 0;
    size_t total = 0;
    ssize_t n;

    while (1) {
        if (total == capacity) {
            size_t new_cap = (capacity == 0) ? RECV_BUFFER_SIZE : capacity * 2;
            char *new_buf = realloc(buffer, new_cap);
            if (!new_buf) {
                free(buffer);
                return NULL;
            }
            buffer = new_buf;
            capacity = new_cap;
        }

        n = read(fd, buffer + total, capacity - total);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            free(buffer);
            return NULL;
        }
        if (n == 0)
            break; /* EOF */

        total += (size_t)n;
    }

    *out_size = total;
    return buffer;
}

/* ==================================================================
 * Fix 6 / Fix 7: Regular-file I/O helpers – compiled only when
 * !USE_AESD_CHAR_DEVICE.
 *
 * The #if block also contains timestamp_thread_func because timestamps
 * are only written to the regular file; the char-device path does not
 * use them.
 * ================================================================== */
#if !USE_AESD_CHAR_DEVICE

/*
 * write_data_to_file - Append data to /var/tmp/aesdsocketdata under mutex.
 *
 * O_CREAT: create the file if it does not exist.
 * O_APPEND: every write goes atomically to the end of the file regardless of
 *   f_pos, which is important for concurrent access even with our mutex.
 * O_CLOEXEC: fd does not survive a fork (see run_as_daemon).
 *
 * Handles partial writes by looping until all bytes are committed.
 */
static int write_data_to_file(const char *data, size_t length)
{
    int fd;
    size_t total_written = 0;

    pthread_mutex_lock(&file_mutex);

    fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    while (total_written < length) {
        ssize_t bytes_written = write(fd, data + total_written,
                                      length - total_written);
        if (bytes_written == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "Failed to write to %s: %s", DATA_FILE, strerror(errno));
            close(fd);
            pthread_mutex_unlock(&file_mutex);
            return -1;
        }
        total_written += (size_t)bytes_written;
    }

    close(fd);
    pthread_mutex_unlock(&file_mutex);
    return 0;
}

/*
 * read_and_send_file - Read the entire regular data file and send to client.
 *
 * Holds file_mutex only while reading into a heap buffer; releases the lock
 * before the (potentially slow) network send.  This prevents a blocked client
 * from stalling concurrent writers.
 */
static int read_and_send_file(int client_fd)
{
    int fd;
    char *file_buffer = NULL;
    size_t file_size = 0;
    int result = 0;

    pthread_mutex_lock(&file_mutex);

    fd = open(DATA_FILE, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        pthread_mutex_unlock(&file_mutex);
        return 0; /* File does not exist yet – nothing to send */
    }

    file_buffer = read_entire_file(fd, &file_size);
    close(fd);

    pthread_mutex_unlock(&file_mutex);

    if (!file_buffer) {
        syslog(LOG_ERR, "Failed to read %s into buffer", DATA_FILE);
        return -1;
    }

    size_t sent = 0;
    while (sent < file_size) {
        ssize_t n = send(client_fd, file_buffer + sent, file_size - sent, 0);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "Failed to send data to client: %s", strerror(errno));
            result = -1;
            break;
        }
        sent += (size_t)n;
    }

    free(file_buffer);
    return result;
}

/*
 * timestamp_thread_func - Write a formatted RFC-2822 timestamp to the data
 * file every TIMESTAMP_INTERVAL seconds.
 *
 * Fix 5 (verified present): Uses localtime_r() instead of localtime().
 * localtime() maintains a single static struct tm buffer inside the C library
 * that is shared across all threads.  Calling it from multiple threads
 * concurrently (or even from a thread while another calls ctime()) produces a
 * data race.  localtime_r() fills a caller-supplied stack variable, making it
 * fully reentrant and MT-safe.  The POSIX-2001 _r convention applies across
 * the API: strtok_r, strerror_r, gmtime_r all follow the same pattern.
 */
static void *timestamp_thread_func(void *arg)
{
    (void)arg; /* unused */

    while (!shutdown_requested) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += TIMESTAMP_INTERVAL;

        pthread_mutex_lock(&timestamp_mutex);
        int ret = pthread_cond_timedwait(&timestamp_cond, &timestamp_mutex, &ts);
        pthread_mutex_unlock(&timestamp_mutex);

        if (ret == ETIMEDOUT && !shutdown_requested) {
            time_t now = time(NULL);
            struct tm tm_buf;                              /* stack storage – reentrant */
            struct tm *tm_info = localtime_r(&now, &tm_buf);
            char timestamp[64];
            strftime(timestamp, sizeof(timestamp),
                     "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);

            write_data_to_file(timestamp, strlen(timestamp));
            syslog(LOG_DEBUG, "Wrote timestamp: %s", timestamp);
        }
    }

    return NULL;
}

#endif /* !USE_AESD_CHAR_DEVICE */


/* ==================================================================
 * Fix 6 / Fix 7: Character-device I/O helpers – compiled only when
 * USE_AESD_CHAR_DEVICE=1.
 *
 * These replace write_data_to_file + read_and_send_file for the device
 * path and add the AESDCHAR_IOCSEEKTO seek path.  Separating them into
 * their own #if block eliminates the dead O_WRONLY-vs-O_APPEND branch
 * that existed inside write_data_to_file in earlier revisions.
 * ================================================================== */
#if USE_AESD_CHAR_DEVICE

/*
 * write_and_readback_chardev - Handle a normal (non-seek) packet for the
 * char-device backend in three phases:
 *
 *   Phase 1 (write, under mutex): Open DATA_FILE O_WRONLY, write packet.
 *   Phase 2 (read,  under mutex): Open DATA_FILE O_RDONLY, read entire
 *                                 device content into a heap buffer.
 *   Phase 3 (send, outside mutex): Send the buffer to the client.
 *
 * Two separate opens are intentional:
 *   - The write open uses O_WRONLY only.  The driver appends data to its
 *     circular buffer regardless of f_pos, so O_APPEND is meaningless on
 *     this char device (the kernel only uses O_APPEND for regular files).
 *   - A fresh O_RDONLY open creates a new file description with f_pos = 0,
 *     giving the client a read from the beginning of all stored data.
 *
 * Both opens use O_CLOEXEC (Fix 13) to prevent the fds from surviving the
 * double-fork in run_as_daemon().
 *
 * The mutex is released before the send so a slow or stalled client does not
 * hold the lock and block concurrent writers.
 */
static int write_and_readback_chardev(int client_fd, const char *data, size_t length)
{
    int wfd;
    size_t total_written = 0;
    int rfd;
    char *file_buffer = NULL;
    size_t file_size = 0;
    int result = 0;

    pthread_mutex_lock(&file_mutex);

    /* ---- Phase 1: Write ---- */
    wfd = open(DATA_FILE, O_WRONLY | O_CLOEXEC);
    if (wfd == -1) {
        syslog(LOG_ERR, "write_and_readback_chardev: open for write failed: %s",
               strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    while (total_written < length) {
        ssize_t n = write(wfd, data + total_written, length - total_written);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "write_and_readback_chardev: write failed: %s",
                   strerror(errno));
            close(wfd);
            pthread_mutex_unlock(&file_mutex);
            return -1;
        }
        total_written += (size_t)n;
    }
    close(wfd);

    /* ---- Phase 2: Read into buffer (still under mutex so no write interleaves) ---- */
    rfd = open(DATA_FILE, O_RDONLY | O_CLOEXEC);
    if (rfd == -1) {
        syslog(LOG_ERR, "write_and_readback_chardev: open for read failed: %s",
               strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    file_buffer = read_entire_file(rfd, &file_size);
    close(rfd);

    pthread_mutex_unlock(&file_mutex);

    if (!file_buffer) {
        syslog(LOG_ERR, "write_and_readback_chardev: read_entire_file failed");
        return -1;
    }

    /* ---- Phase 3: Send (outside lock) ---- */
    size_t sent = 0;
    while (sent < file_size) {
        ssize_t n = send(client_fd, file_buffer + sent, file_size - sent, 0);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "write_and_readback_chardev: send failed: %s",
                   strerror(errno));
            result = -1;
            break;
        }
        sent += (size_t)n;
    }

    free(file_buffer);
    return result;
}

/*
 * handle_seekto_command - Handle a received AESDCHAR_IOCSEEKTO:X,Y command.
 *
 * Spec requirements:
 *   - Parse X (write_cmd) and Y (write_cmd_offset) from the packet string.
 *   - Do NOT write the command string to the device.
 *   - Open the device, issue AESDCHAR_IOCSEEKTO (driver updates filp->f_pos).
 *   - Read from that SAME fd (f_pos is now at the seeked location) into a
 *     heap buffer under file_mutex.
 *   - Release mutex, then send the buffer to the client.
 *
 * Why the same fd must be reused for the read (lecture slide ref):
 *   The kernel file position (f_pos / loff_t) lives inside the "file
 *   description" object, not the integer fd.  Closing the fd and opening a
 *   new one creates a fresh file description with f_pos = 0, discarding the
 *   offset set by the ioctl.  The ioctl and the read must therefore share
 *   the same file description, i.e. the same fd.
 *
 * Fix 4: The mutex is held only across open+ioctl+read_into_buffer+close.
 *   The send happens outside the lock, matching the pattern in
 *   write_and_readback_chardev.
 * Fix 11: Values are validated to fit in uint32_t after strtoul.
 * Fix 12: Trailing garbage after Y is rejected.
 * Fix 13: O_CLOEXEC is used on the open() call.
 */
static int handle_seekto_command(int client_fd, const char *packet)
{
    struct aesd_seekto seekto;
    unsigned long x, y;
    const char *args;
    char *endptr;
    int data_fd;
    char *content = NULL;
    size_t content_size = 0;
    int result = 0;

    /* Skip past "AESDCHAR_IOCSEEKTO:" to reach the "X,Y\n" portion */
    args = packet + strlen(SEEKTO_CMD_PREFIX);

    /*
     * Parse X using strtoul rather than sscanf or atoi:
     *   - atoi: no overflow detection at all.
     *   - sscanf %u: silently truncates on overflow; return value only
     *     confirms a conversion happened, not that the value is in range.
     *   - strtoul: sets errno=ERANGE on overflow; sets endptr==args when
     *     no digits are found.
     *
     * errno must be zeroed before the call.  strtoul only SETS errno on
     * error, it never clears it.  A non-zero errno from a prior syscall
     * would produce a false error report after strtoul.
     */
    errno = 0;
    x = strtoul(args, &endptr, 10);
    if (endptr == args || errno == ERANGE) {
        syslog(LOG_ERR, "handle_seekto_command: invalid write_cmd value");
        return -1;
    }

    /*
     * Fix 11: strtoul returns unsigned long which may be 64 bits on LP64
     * platforms.  The driver struct field is uint32_t, so we must explicitly
     * reject values that would be silently truncated by the cast.
     */
    if (x > UINT32_MAX) {
        syslog(LOG_ERR, "handle_seekto_command: write_cmd out of uint32_t range");
        return -1;
    }

    /* Expect comma separator between X and Y */
    if (*endptr != ',') {
        syslog(LOG_ERR, "handle_seekto_command: missing ',' between X and Y");
        return -1;
    }
    args = endptr + 1;

    /* Parse Y with the same strtoul pattern */
    errno = 0;
    y = strtoul(args, &endptr, 10);
    if (endptr == args || errno == ERANGE) {
        syslog(LOG_ERR, "handle_seekto_command: invalid write_cmd_offset value");
        return -1;
    }

    /* Fix 11: uint32_t range check for Y */
    if (y > UINT32_MAX) {
        syslog(LOG_ERR, "handle_seekto_command: write_cmd_offset out of uint32_t range");
        return -1;
    }

    /*
     * Fix 12: After parsing Y, endptr must point at the packet newline or the
     * NUL terminator written by process_complete_packet.  Any other character
     * means the field has unexpected trailing content (e.g.
     * "AESDCHAR_IOCSEEKTO:0,0garbage\n") and the packet should be rejected
     * rather than silently ignored.
     */
    if (*endptr != '\n' && *endptr != '\0') {
        syslog(LOG_ERR, "handle_seekto_command: unexpected trailing characters after Y");
        return -1;
    }

    seekto.write_cmd        = (uint32_t)x;
    seekto.write_cmd_offset = (uint32_t)y;

    syslog(LOG_DEBUG, "handle_seekto_command: write_cmd=%u write_cmd_offset=%u",
           seekto.write_cmd, seekto.write_cmd_offset);

    /*
     * Fix 4: Hold file_mutex across open -> ioctl -> read_into_buffer -> close.
     * No concurrent write_and_readback_chardev may interleave between the ioctl
     * (which sets f_pos in the kernel) and the read (which uses f_pos).  If a
     * write landed in that window the circular buffer could rotate, invalidating
     * the byte offset the ioctl computed.
     *
     * The device is opened O_RDWR rather than O_RDONLY because the ioctl
     * modifies file state (f_pos).  A conformant driver may reject
     * state-modifying ioctls on a read-only file description.
     *
     * Fix 13: O_CLOEXEC prevents the fd from leaking across fork().
     */
    pthread_mutex_lock(&file_mutex);

    data_fd = open(DATA_FILE, O_RDWR | O_CLOEXEC);
    if (data_fd == -1) {
        syslog(LOG_ERR, "handle_seekto_command: failed to open %s: %s",
               DATA_FILE, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    /*
     * Issue AESDCHAR_IOCSEEKTO.  The driver translates (write_cmd,
     * write_cmd_offset) into an absolute byte offset within its circular
     * buffer and sets filp->f_pos to that value.  Subsequent reads on
     * data_fd will begin from that position.
     */
    if (ioctl(data_fd, AESDCHAR_IOCSEEKTO, &seekto) == -1) {
        syslog(LOG_ERR, "handle_seekto_command: AESDCHAR_IOCSEEKTO ioctl failed: %s",
               strerror(errno));
        close(data_fd);
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    /*
     * Read from the SAME fd (f_pos is now at the seeked offset).  Closing
     * data_fd and opening a new fd would reset f_pos to 0.
     */
    content = read_entire_file(data_fd, &content_size);
    close(data_fd);

    pthread_mutex_unlock(&file_mutex);

    /* Fix 4: Send buffer to client outside the lock */
    if (!content) {
        syslog(LOG_ERR, "handle_seekto_command: read_entire_file failed");
        return -1;
    }

    size_t sent = 0;
    while (sent < content_size) {
        ssize_t n = send(client_fd, content + sent, content_size - sent, 0);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "handle_seekto_command: send failed: %s", strerror(errno));
            result = -1;
            break;
        }
        sent += (size_t)n;
    }

    free(content);
    return result;
}

#endif /* USE_AESD_CHAR_DEVICE */


/*
 * is_seekto_command - Return true if the NUL-terminated packet begins with
 * the AESDCHAR_IOCSEEKTO: prefix.
 *
 * Fix 2: The packet_size parameter has been removed.  In the previous
 * revision it was silently discarded with (void)packet_size, making the
 * signature misleading and producing a -Wunused-parameter warning.  The
 * comparison needs only a NUL-terminated string, which process_complete_packet
 * guarantees before calling this function.  Removing the parameter eliminates
 * both the warning and the false implication that a length is used.
 *
 * strncmp is length-limited to strlen(SEEKTO_CMD_PREFIX) to perform a prefix
 * match only; the packet also contains "X,Y\n" after the prefix.
 */
static bool is_seekto_command(const char *packet)
{
    return strncmp(packet, SEEKTO_CMD_PREFIX, strlen(SEEKTO_CMD_PREFIX)) == 0;
}

/*
 * process_complete_packet - Dispatch a fully received newline-terminated packet.
 *
 * NUL-terminates packet_buffer (using the +1 byte reserved at allocation time)
 * so that is_seekto_command() can call strncmp safely.
 *
 * Fix 3: client_ip is conditionally compiled.  It is only referenced on the
 * USE_AESD_CHAR_DEVICE=1 path (as context for the seekto log message).  When
 * the regular-file backend is active the parameter would be unused and would
 * cause a -Wunused-parameter warning.  Conditional compilation is cleaner than
 * a silent (void) cast because it accurately reflects that the parameter does
 * not exist in the regular-file variant rather than pretending it does.
 */
static int process_complete_packet(int client_fd,
#if USE_AESD_CHAR_DEVICE
                                   const char *client_ip,
#endif
                                   char *packet_buffer, size_t packet_size)
{
    /* NUL-terminate for is_seekto_command; buffer has capacity+1 bytes */
    packet_buffer[packet_size] = '\0';

#if USE_AESD_CHAR_DEVICE
    if (is_seekto_command(packet_buffer)) {
        syslog(LOG_DEBUG, "Received seekto command from %s: %.*s",
               client_ip,
               (int)(packet_size > 0 ? packet_size - 1 : 0),
               packet_buffer);
        return handle_seekto_command(client_fd, packet_buffer);
    }
    /* Normal (non-seek) packet: write to device then echo full content back */
    return write_and_readback_chardev(client_fd, packet_buffer, packet_size);
#else
    /* Regular-file path: append to file then echo full file content back */
    if (write_data_to_file(packet_buffer, packet_size) == 0)
        return read_and_send_file(client_fd);
    return -1;
#endif
}

/*
 * add_thread_to_list - Prepend a new thread node to the management list.
 */
static void add_thread_to_list(pthread_t thread_id, int client_fd,
                               struct sockaddr_in *client_addr)
{
    struct thread_node *new_node = malloc(sizeof(struct thread_node));
    if (!new_node) {
        syslog(LOG_ERR, "Failed to allocate memory for thread node");
        return;
    }

    new_node->thread_id = thread_id;
    new_node->client_fd = client_fd;
    new_node->client_addr = *client_addr;
    new_node->active = true;

    pthread_mutex_lock(&thread_list_mutex);
    new_node->next = thread_list_head;
    thread_list_head = new_node;
    pthread_mutex_unlock(&thread_list_mutex);
}

/*
 * remove_thread_from_list - Remove a thread node from the management list.
 *
 * Uses the indirect-pointer idiom: 'indirect' holds the address of the pointer
 * that points to the current node.  This can be &thread_list_head (for the
 * first node) or &prev->next (for any subsequent node).  Overwriting
 * *indirect with to_free->next splices the node out in one step and handles
 * both the head case and the mid/tail case identically, with no special branch.
 *
 * pthread_equal() is used instead of == because pthread_t is an opaque type
 * that may be a struct on some platforms; direct == comparison would be
 * undefined in that case.
 */
static void remove_thread_from_list(pthread_t thread_id)
{
    pthread_mutex_lock(&thread_list_mutex);

    struct thread_node **indirect = &thread_list_head;
    while (*indirect) {
        if (pthread_equal((*indirect)->thread_id, thread_id)) {
            struct thread_node *to_free = *indirect;
            *indirect = to_free->next;
            free(to_free);
            break;
        }
        indirect = &(*indirect)->next;
    }

    pthread_mutex_unlock(&thread_list_mutex);
}

/*
 * wait_for_all_threads - Join all active connection threads.
 *
 * The join is performed OUTSIDE thread_list_mutex to prevent a deadlock.
 * Each connection thread calls remove_thread_from_list() as its last act,
 * which acquires thread_list_mutex.  If we held that mutex while calling
 * pthread_join(), the thread being joined would block trying to acquire the
 * same mutex to remove itself – a classic self-deadlock.
 *
 * Solution: pop the node from the list under the lock, release the lock, then
 * join (and free) the now-removed node.
 */
static void wait_for_all_threads(void)
{
    struct thread_node *node;

    while (1) {
        pthread_mutex_lock(&thread_list_mutex);
        node = thread_list_head;
        if (node)
            thread_list_head = node->next;
        pthread_mutex_unlock(&thread_list_mutex);

        if (!node)
            break;

        if (node->active)
            pthread_join(node->thread_id, NULL);
        free(node);
    }
}

/*
 * set_socket_timeout - Set SO_RCVTIMEO and SO_SNDTIMEO on a client socket.
 * Prevents connection_handler from blocking indefinitely on a silent client.
 */
static void set_socket_timeout(int fd, int timeout_sec)
{
    struct timeval tv;
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*
 * connection_handler - Handle one client connection in its own thread.
 *
 * Accumulates received bytes into packet_buffer until a '\n' is found, then
 * calls process_complete_packet.  Multiple packets may arrive within a single
 * TCP connection; packet_size is reset to 0 after each dispatch.
 *
 * Fix 9 (comment corrected): The buffer is allocated at buffer_capacity+1
 * bytes so that process_complete_packet can NUL-terminate the packet without
 * a buffer overrun.  buffer_capacity tracks only the usable data space; the
 * +1 slot is silent extra capacity for the NUL terminator alone.
 *
 * Fix 10: recv() uses sizeof(recv_buffer) (the full RECV_BUFFER_SIZE bytes).
 * The previous -1 guard was left over from when recv_buffer itself needed a
 * NUL terminator.  It no longer does: raw bytes are memcpy'd into
 * packet_buffer (which has the +1 NUL slot), so recv_buffer is purely a raw
 * staging area.
 *
 * Fix 14: close(client_fd) is guarded with (client_fd != -1).
 * cleanup_resources() now calls shutdown() only (not close()) on client fds
 * to unblock recv() without closing the fd, leaving the close to this thread.
 */
static void *connection_handler(void *arg)
{
    struct thread_args *thread_args = (struct thread_args *)arg;
    int client_fd = thread_args->client_fd;
    struct sockaddr_in client_addr = thread_args->client_addr;
    free(thread_args);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    set_socket_timeout(client_fd, 5); /* 5-second timeout on receive and send */

    char *packet_buffer = NULL;
    char recv_buffer[RECV_BUFFER_SIZE];
    size_t packet_size = 0;
    size_t buffer_capacity = RECV_BUFFER_SIZE;
    ssize_t bytes_received;

    /*
     * Allocate +1 byte beyond buffer_capacity so process_complete_packet
     * can NUL-terminate without a buffer overrun.  buffer_capacity
     * intentionally excludes this byte so all size comparisons against
     * MAX_PACKET_SIZE and the growth-doubling logic remain correct.
     */
    packet_buffer = malloc(buffer_capacity + 1);
    if (!packet_buffer) {
        syslog(LOG_ERR, "Failed to allocate packet buffer for %s", client_ip);
        close(client_fd);
        remove_thread_from_list(pthread_self());
        return NULL;
    }

    /* Main receive loop */
    while (!shutdown_requested) {
        /* Fix 10: use full recv_buffer; it is a raw byte staging area only */
        bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer), 0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                syslog(LOG_INFO, "Client %s disconnected", client_ip);
            } else if (errno == EINTR) {
                continue;
            } else {
                syslog(LOG_ERR, "Error receiving data from %s: %s",
                       client_ip, strerror(errno));
            }
            break;
        }

        /* Scan the received chunk for newlines, dispatching each complete packet */
        char *current_pos = recv_buffer;
        size_t remaining  = (size_t)bytes_received;

        while (remaining > 0) {
            char *newline_pos = memchr(current_pos, '\n', remaining);
            size_t chunk_size = newline_pos
                ? (size_t)(newline_pos - current_pos) + 1
                : remaining;

            /* Reject packets exceeding the configured size limit */
            if (packet_size + chunk_size > MAX_PACKET_SIZE) {
                syslog(LOG_ERR, "Packet from %s exceeds maximum size", client_ip);
                free(packet_buffer);
                close(client_fd);
                remove_thread_from_list(pthread_self());
                return NULL;
            }

            /* Expand the packet buffer if the new chunk would overflow it */
            if (packet_size + chunk_size > buffer_capacity) {
                size_t new_capacity = buffer_capacity * 2;
                while (new_capacity < packet_size + chunk_size)
                    new_capacity *= 2;
                if (new_capacity > MAX_PACKET_SIZE)
                    new_capacity = MAX_PACKET_SIZE;

                /* +1 preserves the NUL-terminator slot on every reallocation */
                char *new_buffer = realloc(packet_buffer, new_capacity + 1);
                if (!new_buffer) {
                    syslog(LOG_ERR, "Failed to expand packet buffer for %s", client_ip);
                    free(packet_buffer);
                    close(client_fd);
                    remove_thread_from_list(pthread_self());
                    return NULL;
                }
                packet_buffer  = new_buffer;
                buffer_capacity = new_capacity;
            }

            memcpy(packet_buffer + packet_size, current_pos, chunk_size);
            packet_size += chunk_size;
            current_pos += chunk_size;
            remaining   -= chunk_size;

            /* A complete newline-terminated packet has been assembled */
            if (newline_pos) {
                process_complete_packet(client_fd,
#if USE_AESD_CHAR_DEVICE
                                        client_ip,
#endif
                                        packet_buffer, packet_size);
                packet_size = 0; /* Reset for the next packet in this connection */
            }
        }
    }

    free(packet_buffer);

    /*
     * Fix 14: Guard the close.  cleanup_resources() calls shutdown() (not
     * close()) on client fds.  The fd is still open after cleanup_resources
     * runs; this thread is responsible for closing it.  The guard is a
     * safety net in case client_fd somehow became -1, which is not a valid
     * fd to close().
     */
    if (client_fd != -1)
        close(client_fd);

    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    remove_thread_from_list(pthread_self());

    return NULL;
}

/*
 * run_as_daemon - Convert the process to a UNIX daemon via a double-fork.
 *
 * Fork 1: parent exits so init (PID 1) adopts the child.
 * setsid(): make the child a session leader with no controlling terminal.
 * Fork 2: grandchild can never re-acquire a controlling terminal (not a
 *   session leader).
 * stdin/stdout/stderr are redirected to /dev/null to prevent accidental
 * reads or writes from crashing the process.
 */
static int run_as_daemon(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "First fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to create new session: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Second fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (chdir("/") < 0)
        syslog(LOG_WARNING, "Failed to change directory to /: %s", strerror(errno));

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    return 0;
}

/*
 * verify_device_exists - Confirm DATA_FILE is a char device at startup.
 * Fails fast with a clear diagnostic if the driver module is not loaded.
 */
static int verify_device_exists(void)
{
#if USE_AESD_CHAR_DEVICE
    struct stat st;
    if (stat(DATA_FILE, &st) == -1) {
        syslog(LOG_ERR, "Device %s does not exist. Is the driver loaded?", DATA_FILE);
        return -1;
    }
    if (!S_ISCHR(st.st_mode)) {
        syslog(LOG_ERR, "%s is not a character device", DATA_FILE);
        return -1;
    }
#endif
    return 0;
}

/*
 * cleanup_resources - Orderly teardown of all server resources.
 *
 * Fix 14: To prevent a double-close race, this function calls shutdown()
 * (not close()) on each client fd.  shutdown(SHUT_RDWR) causes recv() in the
 * connection thread to return 0 (EOF) so the thread exits its loop.  The
 * thread then calls close(client_fd) itself.  This eliminates the window
 * where cleanup_resources closes the fd, the kernel recycles the fd number,
 * another open() grabs that number, and the thread later closes the wrong fd.
 */
static void cleanup_resources(void)
{
    shutdown_requested = true;
#if !USE_AESD_CHAR_DEVICE
    pthread_cond_signal(&timestamp_cond);
#endif

    /* Close server socket to unblock accept() in the main loop */
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    /*
     * Use shutdown() – not close() – on each client fd.  See Fix 14 note.
     * active is set to false so a subsequent call does not repeat the shutdown.
     */
    pthread_mutex_lock(&thread_list_mutex);
    struct thread_node *current = thread_list_head;
    while (current) {
        if (current->active) {
            shutdown(current->client_fd, SHUT_RDWR);
            /* Deliberately NOT calling close() here – connection_handler owns it */
            current->active = false;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);

#if !USE_AESD_CHAR_DEVICE
    if (timestamp_thread_running)
        pthread_join(timestamp_thread, NULL);
#endif

    wait_for_all_threads();

#if !USE_AESD_CHAR_DEVICE
    if (unlink(DATA_FILE) == -1 && errno != ENOENT)
        syslog(LOG_WARNING, "Failed to remove data file: %s", strerror(errno));
#endif

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&thread_list_mutex);
#if !USE_AESD_CHAR_DEVICE
    pthread_mutex_destroy(&timestamp_mutex);
    pthread_cond_destroy(&timestamp_cond);
#endif

    closelog();
}

/*
 * handle_accept_error - Decide whether an accept() failure is transient.
 *
 * EMFILE/ENFILE: process or system fd table is full – wait and retry.
 * ENOBUFS/ENOMEM: kernel buffer exhaustion – same treatment.
 * Any other error: not recoverable by waiting; return false so the caller
 * can log and continue (the main loop will check shutdown_requested).
 */
static bool handle_accept_error(int err)
{
    switch (err) {
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            syslog(LOG_WARNING, "Accept failed: %s, retrying in %d ms",
                   strerror(err), ACCEPT_RETRY_DELAY_MS);
            usleep(ACCEPT_RETRY_DELAY_MS * 1000);
            return true;
        default:
            return false;
    }
}

/*
 * main - Program entry point
 */
int main(int argc, char *argv[])
{
    struct sockaddr_in client_addr;
    socklen_t client_len;
    pthread_attr_t thread_attr;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = true;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            fprintf(stderr, "  -d    Run as daemon\n");
            return EXIT_FAILURE;
        }
    }

    /*
     * LOG_PID: prepend the process ID to each message – useful when multiple
     *   instances run simultaneously.
     * LOG_CONS: fall back to the system console if syslog is unavailable, so
     *   startup errors are never silently lost.
     */
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Starting aesdsocket%s using endpoint: %s",
           daemon_mode ? " in daemon mode" : "", DATA_FILE);

    if (verify_device_exists() == -1) {
        closelog();
        return EXIT_FAILURE;
    }

    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&thread_list_mutex, NULL);
#if !USE_AESD_CHAR_DEVICE
    pthread_mutex_init(&timestamp_mutex, NULL);
    pthread_cond_init(&timestamp_cond, NULL);
#endif

    if (setup_signal_handlers() == -1) {
        cleanup_resources();
        return EXIT_FAILURE;
    }

    server_fd = setup_server_socket();
    if (server_fd == -1) {
        cleanup_resources();
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        if (run_as_daemon() == -1) {
            cleanup_resources();
            return EXIT_FAILURE;
        }
    }

#if !USE_AESD_CHAR_DEVICE
    if (pthread_create(&timestamp_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread: %s", strerror(errno));
        cleanup_resources();
        return EXIT_FAILURE;
    }
    timestamp_thread_running = true;
#endif

    /*
     * PTHREAD_CREATE_JOINABLE is the default on Linux but is set explicitly
     * for portability and clarity: we must be able to join threads in
     * wait_for_all_threads(), which requires joinable state.
     */
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    /* Main accept loop */
    while (!shutdown_requested) {
        client_len = sizeof(client_addr);
        int client_fd;

        /*
         * accept4() with SOCK_CLOEXEC atomically sets close-on-exec on the
         * new fd, closing the race window that exists between accept() and a
         * separate fcntl(F_SETFD, FD_CLOEXEC) call.
         */
#ifdef SOCK_CLOEXEC
        client_fd = accept4(server_fd, (struct sockaddr *)&client_addr,
                            &client_len, SOCK_CLOEXEC);
#else
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
#endif

        if (client_fd == -1) {
            if (errno == EINTR && shutdown_requested)
                break;
            if (handle_accept_error(errno))
                continue;
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }

        struct thread_args *args = malloc(sizeof(struct thread_args));
        if (!args) {
            syslog(LOG_ERR, "Failed to allocate thread arguments");
            close(client_fd);
            continue;
        }
        args->client_fd   = client_fd;
        args->client_addr = client_addr;

        pthread_t thread_id;
        if (pthread_create(&thread_id, &thread_attr, connection_handler, args) != 0) {
            syslog(LOG_ERR, "Failed to create connection thread: %s", strerror(errno));
            free(args);
            close(client_fd);
            continue;
        }

        add_thread_to_list(thread_id, client_fd, &client_addr);
    }

    /*
     * Fix 8: pthread_attr_destroy releases resources allocated by
     * pthread_attr_init.  It is called after the loop exits (no further
     * pthread_create calls will be made) and before cleanup_resources() so
     * the attribute object's lifetime is correctly bounded.
     */
    pthread_attr_destroy(&thread_attr);

    cleanup_resources();
    syslog(LOG_INFO, "Server shutdown complete");

    return EXIT_SUCCESS;
}
