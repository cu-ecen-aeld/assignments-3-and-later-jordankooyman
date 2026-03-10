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
 * 
 *  Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
 *	Version 2 Code (this): https://chat.deepseek.com/share/qtyyz0zhqx67gk3lir
 *   Comparison: https://chatgpt.com/share/697cea7e-2700-8007-8fa5-a7edea60a08d
 * 
 * Refactored for Assignment 7 with the following improvements:
 *   - Fixed thread join deadlock by joining threads outside list mutex
 *   - Made signal handler async-signal-safe
 *   - Added partial write handling in file I/O
 *   - Avoid holding file mutex during network send (reads entire file into buffer under lock)
 *   - Verify /dev/aesdchar exists at startup when enabled
 *   - Added optional accept4() with CLOEXEC, accept error handling, and socket timeouts
 * 
 *  Version 1 Update (this): https://chat.deepseek.com/share/zwsr2jih5tudk5thqa
 *	Version 2 Update: https://chat.deepseek.com/share/h6pac8gevqhneunhdq
 *   Comparison: https://chatgpt.com/share/69a0c8e6-0d4c-8007-a1c3-f982931753fe
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
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ==================== Build Configuration ==================== */
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1   /* Default to using the character driver */
#endif

/* Select the data endpoint based on the build switch */
#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"          /* Character device */
#else
#define DATA_FILE "/var/tmp/aesdsocketdata" /* Regular file */
#endif
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

/* Forward declarations */
static void signal_handler(int signal);
static int setup_signal_handlers(void);
static int setup_server_socket(void);
static void cleanup_resources(void);
static void *connection_handler(void *arg);
#if !USE_AESD_CHAR_DEVICE
static void *timestamp_thread_func(void *arg);
#endif
static int run_as_daemon(void);
static int write_data_to_file(const char *data, size_t length);
static int read_and_send_file(int client_fd);
static void add_thread_to_list(pthread_t thread_id, int client_fd, struct sockaddr_in *client_addr);
static void remove_thread_from_list(pthread_t thread_id);
static void wait_for_all_threads(void);

/*
 * signal_handler - Handle SIGINT and SIGTERM signals
 *
 * Async-signal-safe: only sets a flag and shuts down the server socket.
 * All other cleanup (logging, waking threads) is done in main/cleanup.
 */
static void signal_handler(int signal)
{
    (void)signal; /* unused */
    shutdown_requested = 1;

    /* Shutdown server socket to unblock accept() */
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

/*
 * setup_signal_handlers - Register signal handlers
 */
static int setup_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* no SA_RESTART to allow accept() to return EINTR */

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
 * setup_server_socket - Create and configure server socket
 * Uses accept4() with SOCK_CLOEXEC if available to avoid fd leaks.
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

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_WARNING, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

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
 * write_data_to_file - Append data to the configured endpoint with mutex protection
 * Handles partial writes by looping until all bytes are written.
 */
static int write_data_to_file(const char *data, size_t length)
{
    int fd;
    size_t total_written = 0;  /* use size_t to avoid signed/unsigned compare */
    int flags;

    pthread_mutex_lock(&file_mutex);

#if USE_AESD_CHAR_DEVICE
    flags = O_WRONLY;               /* Device node exists, no creation, no append flag needed */
#else
    flags = O_WRONLY | O_CREAT | O_APPEND; /* Regular file: create if missing, always append */
#endif

    fd = open(DATA_FILE, flags, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    /* Write all data, retrying on partial writes and EINTR */
    while (total_written < length) {
        ssize_t bytes_written = write(fd, data + total_written, length - total_written);
        if (bytes_written == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "Failed to write to %s: %s", DATA_FILE, strerror(errno));
            close(fd);
            pthread_mutex_unlock(&file_mutex);
            return -1;
        }
        total_written += bytes_written;
    }

    close(fd);
    pthread_mutex_unlock(&file_mutex);
    return 0;
}

/*
 * read_entire_file - Read entire content of file/device into a dynamically allocated buffer.
 * Returns pointer to buffer (must be freed by caller) and sets out_size.
 * On error, returns NULL.
 * Caller must hold file_mutex when calling this function.
 */
static char *read_entire_file(int fd, size_t *out_size)
{
    char *buffer = NULL;
    size_t capacity = 0;
    size_t total = 0;
    ssize_t n;

    while (1) {
        if (total == capacity) {
            size_t new_cap = capacity == 0 ? RECV_BUFFER_SIZE : capacity * 2;
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
            break;  /* EOF */

        total += n;
    }

    *out_size = total;
    return buffer;
}

/*
 * read_and_send_file - Read entire file/device and send to client.
 * Holds file mutex only while reading into a temporary buffer; then releases
 * mutex and sends from buffer. This avoids blocking other writers during
 * network I/O while providing a consistent snapshot.
 */
static int read_and_send_file(int client_fd)
{
    int fd;
    char *file_buffer = NULL;
    size_t file_size = 0;
    int result = 0;

    pthread_mutex_lock(&file_mutex);

    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        pthread_mutex_unlock(&file_mutex);
        return 0; /* File doesn't exist yet */
    }

    /* Read entire file into a buffer while holding the lock */
    file_buffer = read_entire_file(fd, &file_size);
    close(fd);

    pthread_mutex_unlock(&file_mutex);

    if (!file_buffer) {
        syslog(LOG_ERR, "Failed to read %s into buffer", DATA_FILE);
        return -1;
    }

    /* Send the buffer to client (mutex is now released) */
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
        sent += n;
    }

    free(file_buffer);
    return result;
}

/*
 * add_thread_to_list - Add new thread to linked list
 */
static void add_thread_to_list(pthread_t thread_id, int client_fd, struct sockaddr_in *client_addr)
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
 * remove_thread_from_list - Remove thread from linked list
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
 * wait_for_all_threads - Wait for all active threads to complete.
 *
 * Fixes deadlock by not holding the mutex while joining.
 * For each node in the list, we remove it under lock, then join and free
 * outside the lock. This ensures that a thread trying to remove itself
 * (which holds the same mutex) does not deadlock.
 */
static void wait_for_all_threads(void)
{
    struct thread_node *node;

    while (1) {
        pthread_mutex_lock(&thread_list_mutex);
        node = thread_list_head;
        if (node) {
            thread_list_head = node->next;
        }
        pthread_mutex_unlock(&thread_list_mutex);

        if (!node)
            break;

        if (node->active) {
            pthread_join(node->thread_id, NULL);
        }
        free(node);
    }
}

#if !USE_AESD_CHAR_DEVICE
/*
 * timestamp_thread_func - Thread function that writes timestamps every 10 seconds
 * (Only compiled when NOT using the character device)
 */
static void *timestamp_thread_func(void *arg)
{
    (void)arg; /* Unused parameter */

    while (!shutdown_requested) {
        /* Wait for 10 seconds or until signaled */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += TIMESTAMP_INTERVAL;

        pthread_mutex_lock(&timestamp_mutex);
        int ret = pthread_cond_timedwait(&timestamp_cond, &timestamp_mutex, &ts);
        pthread_mutex_unlock(&timestamp_mutex);

        /* If we timed out (not signaled), write timestamp */
        if (ret == ETIMEDOUT && !shutdown_requested) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char timestamp[64];
            strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);

            write_data_to_file(timestamp, strlen(timestamp));
            syslog(LOG_DEBUG, "Wrote timestamp: %s", timestamp);
        }
    }

    return NULL;
}
#endif /* !USE_AESD_CHAR_DEVICE */

/*
 * set_socket_timeout - Set receive and send timeouts on a socket.
 * Optional helper to prevent hanging on stuck clients.
 */
static void set_socket_timeout(int fd, int timeout_sec)
{
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*
 * connection_handler - Handle individual client connection
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

    /* Set timeouts on client socket to avoid hanging */
    set_socket_timeout(client_fd, 5); /* 5 seconds timeout */

    char *packet_buffer = NULL;
    char recv_buffer[RECV_BUFFER_SIZE];
    size_t packet_size = 0;
    size_t buffer_capacity = RECV_BUFFER_SIZE;
    ssize_t bytes_received;

    /* Allocate initial packet buffer */
    packet_buffer = malloc(buffer_capacity);
    if (!packet_buffer) {
        syslog(LOG_ERR, "Failed to allocate packet buffer for %s", client_ip);
        close(client_fd);
        remove_thread_from_list(pthread_self());
        return NULL;
    }

    /* Main connection loop */
    while (!shutdown_requested) {
        bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer) - 1, 0);

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

        /* Process received data */
        char *current_pos = recv_buffer;
        size_t remaining = bytes_received;

        while (remaining > 0) {
            char *newline_pos = memchr(current_pos, '\n', remaining);
            size_t chunk_size = newline_pos ? (size_t)(newline_pos - current_pos) + 1 : remaining;

            /* Check packet size limit */
            if (packet_size + chunk_size > MAX_PACKET_SIZE) {
                syslog(LOG_ERR, "Packet from %s exceeds maximum size", client_ip);
                free(packet_buffer);
                close(client_fd);
                remove_thread_from_list(pthread_self());
                return NULL;
            }

            /* Expand buffer if needed */
            if (packet_size + chunk_size > buffer_capacity) {
                size_t new_capacity = buffer_capacity * 2;
                while (new_capacity < packet_size + chunk_size) {
                    new_capacity *= 2;
                }
                if (new_capacity > MAX_PACKET_SIZE) {
                    new_capacity = MAX_PACKET_SIZE;
                }

                char *new_buffer = realloc(packet_buffer, new_capacity);
                if (!new_buffer) {
                    syslog(LOG_ERR, "Failed to expand packet buffer for %s", client_ip);
                    free(packet_buffer);
                    close(client_fd);
                    remove_thread_from_list(pthread_self());
                    return NULL;
                }
                packet_buffer = new_buffer;
                buffer_capacity = new_capacity;
            }

            /* Copy data to packet buffer */
            memcpy(packet_buffer + packet_size, current_pos, chunk_size);
            packet_size += chunk_size;

            current_pos += chunk_size;
            remaining -= chunk_size;

            /* If we found a newline, process the complete packet */
            if (newline_pos) {
                if (write_data_to_file(packet_buffer, packet_size) == 0) {
                    read_and_send_file(client_fd);
                }
                packet_size = 0; /* Reset for next packet */
            }
        }
    }

    /* Cleanup */
    free(packet_buffer);
    close(client_fd);

    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    remove_thread_from_list(pthread_self());

    return NULL;
}

/*
 * run_as_daemon - Convert process to daemon
 */
static int run_as_daemon(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "First fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to create new session: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Second fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (chdir("/") < 0) {
        syslog(LOG_WARNING, "Failed to change directory to /: %s", strerror(errno));
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    return 0;
}

/*
 * verify_device_exists - Check that the data endpoint exists when using character device.
 * Called at startup.
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
 * cleanup_resources - Clean up all resources
 */
static void cleanup_resources(void)
{
    /* Signal shutdown and wake up timestamp thread if it exists */
    shutdown_requested = true;
#if !USE_AESD_CHAR_DEVICE
    pthread_cond_signal(&timestamp_cond);
#endif

    /* Close server socket */
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    /* Close all client sockets to wake up connection threads */
    pthread_mutex_lock(&thread_list_mutex);
    struct thread_node *current = thread_list_head;
    while (current) {
        if (current->active) {
            shutdown(current->client_fd, SHUT_RDWR);
            close(current->client_fd);
            current->active = false;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);

#if !USE_AESD_CHAR_DEVICE
    /* Wait for timestamp thread */
    if (timestamp_thread_running) {
        pthread_join(timestamp_thread, NULL);
    }
#endif

    /* Wait for all connection threads */
    wait_for_all_threads();

#if !USE_AESD_CHAR_DEVICE
    /* Remove the regular data file (not needed/desired for character device) */
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_WARNING, "Failed to remove data file: %s", strerror(errno));
    }
#endif

    /* Destroy mutexes and condition variable (if used) */
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&thread_list_mutex);
#if !USE_AESD_CHAR_DEVICE
    pthread_mutex_destroy(&timestamp_mutex);
    pthread_cond_destroy(&timestamp_cond);
#endif

    closelog();
}

/*
 * handle_accept_error - Handle accept() errors like EMFILE/ENFILE with delay.
 * Returns true if the error is temporary and we should retry, false otherwise.
 */
static bool handle_accept_error(int err)
{
    switch (err) {
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            /* Temporary resource shortage, wait a bit and retry */
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

    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = true;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            fprintf(stderr, "  -d    Run as daemon\n");
            return EXIT_FAILURE;
        }
    }

    /* Initialize syslog */
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Starting aesdsocket%s using endpoint: %s",
           daemon_mode ? " in daemon mode" : "", DATA_FILE);

    /* Verify device exists if using character driver */
    if (verify_device_exists() == -1) {
        closelog();
        return EXIT_FAILURE;
    }

    /* Initialize mutexes and condition variable (if needed) */
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&thread_list_mutex, NULL);
#if !USE_AESD_CHAR_DEVICE
    pthread_mutex_init(&timestamp_mutex, NULL);
    pthread_cond_init(&timestamp_cond, NULL);
#endif

    /* Set up signal handlers */
    if (setup_signal_handlers() == -1) {
        cleanup_resources();
        return EXIT_FAILURE;
    }

    /* Set up server socket */
    server_fd = setup_server_socket();
    if (server_fd == -1) {
        cleanup_resources();
        return EXIT_FAILURE;
    }

    /* Run as daemon if requested */
    if (daemon_mode) {
        if (run_as_daemon() == -1) {
            cleanup_resources();
            return EXIT_FAILURE;
        }
    }

#if !USE_AESD_CHAR_DEVICE
    /* Create timestamp thread only when using regular file */
    if (pthread_create(&timestamp_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread: %s", strerror(errno));
        cleanup_resources();
        return EXIT_FAILURE;
    }
    timestamp_thread_running = true;
#endif

    /* Initialize thread attributes for joinable threads */
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    /* Main server loop */
    while (!shutdown_requested) {
        client_len = sizeof(client_addr);
        int client_fd;

#ifdef SOCK_CLOEXEC
        client_fd = accept4(server_fd, (struct sockaddr *)&client_addr, &client_len,
                            SOCK_CLOEXEC);
#else
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
#endif

        if (client_fd == -1) {
            if (errno == EINTR && shutdown_requested) {
                break;
            }
            if (handle_accept_error(errno)) {
                continue;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }

        /* Allocate thread arguments */
        struct thread_args *args = malloc(sizeof(struct thread_args));
        if (!args) {
            syslog(LOG_ERR, "Failed to allocate thread arguments");
            close(client_fd);
            continue;
        }
        args->client_fd = client_fd;
        args->client_addr = client_addr;

        /* Create thread to handle connection */
        pthread_t thread_id;
        if (pthread_create(&thread_id, &thread_attr, connection_handler, args) != 0) {
            syslog(LOG_ERR, "Failed to create connection thread: %s", strerror(errno));
            free(args);
            close(client_fd);
            continue;
        }

        /* Add thread to management list */
        add_thread_to_list(thread_id, client_fd, &client_addr);
    }

    /* Cleanup and exit */
    cleanup_resources();
    syslog(LOG_INFO, "Server shutdown complete");

    return EXIT_SUCCESS;
}
