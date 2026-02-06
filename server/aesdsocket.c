/*
 * aesdsocket.c - Socket server that receives and stores data packets
 * Enhanced with thread management and timestamps
 * 
 * Features:
 * - Listens on port 9000 for TCP connections
 * - Receives data until newline, appends to /var/tmp/aesdsocketdata
 * - Returns entire file content after each complete packet
 * - Supports daemon mode with -d flag
 * - Supports multiple simultaneous socket connections with threads
 * - Graceful shutdown on SIGINT/SIGTERM (including all spawned threads)
 * - Includes timestamps written to output file every 10 seconds
 * - Cross-platform compatible for x86_64 and ARM64
 * 
 * 	Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
 *	Version 2 Code (this): https://chat.deepseek.com/share/qtyyz0zhqx67gk3lir
 *   Comparison: https://chatgpt.com/share/697cea7e-2700-8007-8fa5-a7edea60a08d
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Configuration constants */
#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_BUFFER_SIZE 1024
#define MAX_PACKET_SIZE (10 * 1024 * 1024)
#define TIMESTAMP_INTERVAL 10 /* seconds */

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
static pthread_t timestamp_thread;
static bool timestamp_thread_running = false;
static pthread_mutex_t timestamp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timestamp_cond = PTHREAD_COND_INITIALIZER;
static bool daemon_mode = false;

/* Forward declarations */
static void signal_handler(int signal);
static int setup_signal_handlers(void);
static int setup_server_socket(void);
static void cleanup_resources(void);
static void *connection_handler(void *arg);
static void *timestamp_thread_func(void *arg);
static int run_as_daemon(void);
static int write_data_to_file(const char *data, size_t length);
static int read_and_send_file(int client_fd);
static void add_thread_to_list(pthread_t thread_id, int client_fd, struct sockaddr_in *client_addr);
static void remove_thread_from_list(pthread_t thread_id);
static void wait_for_all_threads(void);

/*
 * signal_handler - Handle SIGINT and SIGTERM signals
 */
static void signal_handler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        shutdown_requested = 1;
        
        /* Signal timestamp thread */
        pthread_cond_signal(&timestamp_cond);
        
        /* Close server socket to unblock accept() */
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
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
 * setup_server_socket - Create and configure server socket
 */
static int setup_server_socket(void)
{
    int sock_fd;
    struct sockaddr_in server_addr;
    int opt = 1;
    
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
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
 * write_data_to_file - Append data to file with mutex protection
 */
static int write_data_to_file(const char *data, size_t length)
{
    int fd;
    ssize_t bytes_written;
    
    pthread_mutex_lock(&file_mutex);
    
    fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "Failed to open data file: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }
    
    bytes_written = write(fd, data, length);
    if (bytes_written == -1 || (size_t)bytes_written != length) {
        syslog(LOG_ERR, "Failed to write to data file: %s", strerror(errno));
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }
    
    close(fd);
    pthread_mutex_unlock(&file_mutex);
    return 0;
}

/*
 * read_and_send_file - Read entire file and send to client with mutex protection
 */
static int read_and_send_file(int client_fd)
{
    int fd;
    char buffer[RECV_BUFFER_SIZE];
    ssize_t bytes_read, bytes_sent, total_sent;
    
    pthread_mutex_lock(&file_mutex);
    
    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        pthread_mutex_unlock(&file_mutex);
        return 0; /* File doesn't exist yet */
    }
    
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        total_sent = 0;
        
        while (total_sent < bytes_read) {
            bytes_sent = send(client_fd, buffer + total_sent, 
                             bytes_read - total_sent, 0);
            
            if (bytes_sent == -1) {
                if (errno == EINTR) {
                    continue;
                }
                syslog(LOG_ERR, "Failed to send data to client: %s", strerror(errno));
                close(fd);
                pthread_mutex_unlock(&file_mutex);
                return -1;
            }
            
            total_sent += bytes_sent;
        }
    }
    
    if (bytes_read == -1) {
        syslog(LOG_ERR, "Failed to read data file: %s", strerror(errno));
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }
    
    close(fd);
    pthread_mutex_unlock(&file_mutex);
    return 0;
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
 * wait_for_all_threads - Wait for all active threads to complete
 */
static void wait_for_all_threads(void)
{
    pthread_mutex_lock(&thread_list_mutex);
    
    struct thread_node *current = thread_list_head;
    while (current) {
        if (current->active) {
            pthread_join(current->thread_id, NULL);
            current->active = false;
        }
        struct thread_node *to_free = current;
        current = current->next;
        free(to_free);
    }
    
    thread_list_head = NULL;
    pthread_mutex_unlock(&thread_list_mutex);
}

/*
 * timestamp_thread_func - Thread function that writes timestamps every 10 seconds
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
 * cleanup_resources - Clean up all resources
 */
static void cleanup_resources(void)
{
    /* Signal shutdown and wake up timestamp thread */
    shutdown_requested = true;
    pthread_cond_signal(&timestamp_cond);
    
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
    
    /* Wait for timestamp thread */
    if (timestamp_thread_running) {
        pthread_join(timestamp_thread, NULL);
    }
    
    /* Wait for all connection threads */
    wait_for_all_threads();
    
    /* Remove data file */
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_WARNING, "Failed to remove data file: %s", strerror(errno));
    }
    
    /* Destroy mutexes and condition variable */
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&thread_list_mutex);
    pthread_mutex_destroy(&timestamp_mutex);
    pthread_cond_destroy(&timestamp_cond);
    
    closelog();
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
    syslog(LOG_INFO, "Starting aesdsocket%s", daemon_mode ? " in daemon mode" : "");
    
    /* Initialize mutexes and condition variable */
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&thread_list_mutex, NULL);
    pthread_mutex_init(&timestamp_mutex, NULL);
    pthread_cond_init(&timestamp_cond, NULL);
    
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
    
    /* Create timestamp thread */
    if (pthread_create(&timestamp_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread: %s", strerror(errno));
        cleanup_resources();
        return EXIT_FAILURE;
    }
    timestamp_thread_running = true;
    
    /* Initialize thread attributes for joinable threads */
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
    
    syslog(LOG_INFO, "Server listening on port %d", PORT);
    
    /* Main server loop */
    while (!shutdown_requested) {
        client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd == -1) {
            if (errno == EINTR && shutdown_requested) {
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            }
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
