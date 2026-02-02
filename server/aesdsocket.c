/*
 * aesdsocket.c - Socket server that receives and stores data packets
 * 
 * Features:
 * - Listens on port 9000 for TCP connections
 * - Receives data until newline, appends to /var/tmp/aesdsocketdata
 * - Returns entire file content after each complete packet
 * - Supports daemon mode with -d flag
 * - Graceful shutdown on SIGINT/SIGTERM
 * - Cross-platform compatible for x86_64 and ARM64
 * 
 * 	Version 1 Code: https://chat.deepseek.com/share/92ytxo7wnlhuiigbbf
 *	Version 2 Code (this): https://chat.deepseek.com/share/v6z76kub49dzcpxmqa
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

/* Configuration constants */
#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define RECV_BUFFER_SIZE 1024
#define MAX_PACKET_SIZE (10 * 1024 * 1024) /* 10MB max packet size */
#define MAX_CONCURRENT_THREADS 10          /* Limit concurrent connections */

/* Thread management structure */
typedef struct {
    pthread_t thread;
    bool active;
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
    uint64_t thread_id;
} thread_info_t;

/* Thread argument structure */
struct thread_args {
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
    uint64_t thread_id;
};

/* Global variables for signal handling and cleanup */
static volatile sig_atomic_t shutdown_requested = 0;
static int server_fd = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t threads_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_info_t thread_pool[MAX_CONCURRENT_THREADS];
static int active_thread_count = 0;
static bool daemon_mode = false;

/* Forward declarations */
static void signal_handler(int signal);
static int setup_signal_handlers(void);
static int setup_server_socket(void);
static void cleanup_resources(void);
static void *connection_handler(void *arg);
static int run_as_daemon(void);
static int write_data_to_file(const char *data, size_t length);
static int read_and_send_file(int client_fd);
static void wait_for_all_threads(void);
static bool can_accept_new_connection(void);
static void register_thread(pthread_t thread, int client_fd, const char *client_ip, uint64_t thread_id);
static void unregister_thread(pthread_t thread);

/*
 * signal_handler - Handle SIGINT and SIGTERM signals
 */
static void signal_handler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        shutdown_requested = 1;
        
        /* Signal main thread by closing server socket */
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

/*
 * setup_signal_handlers - Register signal handlers for graceful shutdown
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
 * setup_server_socket - Create and configure the server socket
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
    
    if (listen(sock_fd, MAX_CONCURRENT_THREADS) == -1) {
        syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
        close(sock_fd);
        return -1;
    }
    
    return sock_fd;
}

/*
 * write_data_to_file - Append data to the data file with thread safety
 */
static int write_data_to_file(const char *data, size_t length)
{
    int fd;
    ssize_t bytes_written;
    
    /* Lock file mutex to ensure thread-safe file access */
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
 * read_and_send_file - Read entire data file and send to client with thread safety
 */
static int read_and_send_file(int client_fd)
{
    int fd;
    char buffer[RECV_BUFFER_SIZE];
    ssize_t bytes_read, bytes_sent, total_sent;
    
    /* Lock file mutex to ensure thread-safe file access */
    pthread_mutex_lock(&file_mutex);
    
    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        /* File might not exist yet, that's OK - just send nothing */
        pthread_mutex_unlock(&file_mutex);
        return 0;
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
 * can_accept_new_connection - Check if we can accept a new connection
 */
static bool can_accept_new_connection(void)
{
    bool can_accept;
    pthread_mutex_lock(&threads_mutex);
    can_accept = (active_thread_count < MAX_CONCURRENT_THREADS);
    pthread_mutex_unlock(&threads_mutex);
    return can_accept;
}

/*
 * register_thread - Register a new active thread
 */
static void register_thread(pthread_t thread, int client_fd, const char *client_ip, uint64_t thread_id)
{
    int i;
    pthread_mutex_lock(&threads_mutex);
    
    /* Find free slot in thread pool */
    for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
        if (!thread_pool[i].active) {
            thread_pool[i].thread = thread;
            thread_pool[i].active = true;
            thread_pool[i].client_fd = client_fd;
            thread_pool[i].thread_id = thread_id;
            if (client_ip) {
                snprintf(thread_pool[i].client_ip, sizeof(thread_pool[i].client_ip),
                        "%s", client_ip);
            }
            active_thread_count++;
            break;
        }
    }
    
    pthread_mutex_unlock(&threads_mutex);
}

/*
 * unregister_thread - Unregister a completed thread
 */
static void unregister_thread(pthread_t thread)
{
    int i;
    pthread_mutex_lock(&threads_mutex);
    
    for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
        if (thread_pool[i].active && pthread_equal(thread_pool[i].thread, thread)) {
            thread_pool[i].active = false;
            active_thread_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&threads_mutex);
}

/*
 * wait_for_all_threads - Wait for all active threads to complete
 */
static void wait_for_all_threads(void)
{
    int i;
    pthread_mutex_lock(&threads_mutex);
    
    for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
        if (thread_pool[i].active) {
            pthread_join(thread_pool[i].thread, NULL);
            thread_pool[i].active = false;
        }
    }
    
    active_thread_count = 0;
    pthread_mutex_unlock(&threads_mutex);
}

/*
 * connection_handler - Handle individual client connection
 */
static void *connection_handler(void *arg)
{
    struct thread_args *thread_args = (struct thread_args *)arg;
    
    int client_fd = thread_args->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    uint64_t thread_id = thread_args->thread_id;
    
    /* Copy client IP safely */
    strncpy(client_ip, thread_args->client_ip, sizeof(client_ip) - 1);
    client_ip[sizeof(client_ip) - 1] = '\0';
    
    free(thread_args);
    
    char *packet_buffer = NULL;
    char recv_buffer[RECV_BUFFER_SIZE];
    size_t packet_size = 0;
    size_t buffer_capacity = RECV_BUFFER_SIZE;
    ssize_t bytes_received;
    bool connection_active = true;
    
    syslog(LOG_INFO, "[Thread:%lu Client:%s] Accepted connection", 
           (unsigned long)thread_id, client_ip);
    
    /* Allocate initial packet buffer */
    packet_buffer = malloc(buffer_capacity);
    if (!packet_buffer) {
        syslog(LOG_ERR, "[Thread:%lu Client:%s] Failed to allocate packet buffer", 
               (unsigned long)thread_id, client_ip);
        close(client_fd);
        unregister_thread(pthread_self());
        return NULL;
    }
    
    /* Main connection loop */
    while (connection_active && !shutdown_requested) {
        bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                /* Client disconnected gracefully */
                syslog(LOG_INFO, "[Thread:%lu Client:%s] Client disconnected", 
                       (unsigned long)thread_id, client_ip);
            } else if (errno == EINTR) {
                /* Interrupted by signal, check shutdown flag */
                continue;
            } else {
                syslog(LOG_ERR, "[Thread:%lu Client:%s] Error receiving data: %s", 
                       (unsigned long)thread_id, client_ip, strerror(errno));
            }
            connection_active = false;
            break;
        }
        
        /* Process received data in place */
        char *current_pos = recv_buffer;
        size_t remaining = bytes_received;
        
        while (remaining > 0 && !shutdown_requested) {
            /* Find newline in remaining data */
            char *newline_pos = memchr(current_pos, '\n', remaining);
            size_t chunk_size;
            
            if (newline_pos) {
                /* Found newline - include it in the packet */
                chunk_size = (newline_pos - current_pos) + 1;
            } else {
                /* No newline - take all remaining data */
                chunk_size = remaining;
            }
            
            /* Check if adding this chunk would exceed max packet size */
            if (packet_size + chunk_size > MAX_PACKET_SIZE) {
                syslog(LOG_ERR, "[Thread:%lu Client:%s] Packet exceeds maximum size (%zu + %zu > %d), discarding", 
                       (unsigned long)thread_id, client_ip, packet_size, chunk_size, MAX_PACKET_SIZE);
                free(packet_buffer);
                close(client_fd);
                unregister_thread(pthread_self());
                return NULL;
            }
            
            /* Ensure buffer has enough capacity */
            if (packet_size + chunk_size > buffer_capacity) {
                size_t new_capacity = buffer_capacity * 2;
                while (new_capacity < packet_size + chunk_size) {
                    new_capacity *= 2;
                }
                
                /* Cap at MAX_PACKET_SIZE */
                if (new_capacity > MAX_PACKET_SIZE) {
                    new_capacity = MAX_PACKET_SIZE;
                }
                
                char *new_buffer = realloc(packet_buffer, new_capacity);
                if (!new_buffer) {
                    syslog(LOG_ERR, "[Thread:%lu Client:%s] Failed to expand packet buffer", 
                           (unsigned long)thread_id, client_ip);
                    free(packet_buffer);
                    close(client_fd);
                    unregister_thread(pthread_self());
                    return NULL;
                }
                
                packet_buffer = new_buffer;
                buffer_capacity = new_capacity;
            }
            
            /* Copy chunk to packet buffer */
            memcpy(packet_buffer + packet_size, current_pos, chunk_size);
            packet_size += chunk_size;
            
            /* Move to next chunk */
            current_pos += chunk_size;
            remaining -= chunk_size;
            
            /* If we found a newline, process the complete packet */
            if (newline_pos) {
                /* Write complete packet to file */
                if (write_data_to_file(packet_buffer, packet_size) != 0) {
                    syslog(LOG_ERR, "[Thread:%lu Client:%s] Failed to write packet to file",
                           (unsigned long)thread_id, client_ip);
                } else {
                    /* Send entire file contents back to client */
                    if (read_and_send_file(client_fd) == -1) {
                        syslog(LOG_ERR, "[Thread:%lu Client:%s] Failed to send file contents", 
                               (unsigned long)thread_id, client_ip);
                    }
                }
                
                /* Reset for next packet */
                packet_size = 0;
            }
        }
    }
    
    /* Cleanup and close connection */
    free(packet_buffer);
    close(client_fd);
    
    syslog(LOG_INFO, "[Thread:%lu Client:%s] Closed connection", 
           (unsigned long)thread_id, client_ip);
    
    unregister_thread(pthread_self());
    return NULL;
}

/*
 * run_as_daemon - Convert process to daemon
 */
static int run_as_daemon(void)
{
    pid_t pid;
    
    pid = fork();
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
    
    /* Close all open file descriptors except server socket */
    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        if (fd != server_fd) {
            close(fd);
        }
    }
    
    /* Reopen stdin, stdout, stderr to /dev/null */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd != -1) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
    }
    
    return 0;
}

/*
 * cleanup_resources - Clean up all resources before exit
 */
static void cleanup_resources(void)
{
    /* Wait for all active threads to complete */
    wait_for_all_threads();
    
    /* Close server socket if open */
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    
    /* Remove data file only after all threads have completed */
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_WARNING, "Failed to remove data file: %s", strerror(errno));
    }
    
    /* Destroy mutexes */
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&threads_mutex);
    
    /* Close syslog */
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
    
    /* Initialize thread pool */
    memset(thread_pool, 0, sizeof(thread_pool));
    
    /* Set up signal handlers */
    if (setup_signal_handlers() == -1) {
        return EXIT_FAILURE;
    }
    
    /* Set up server socket */
    server_fd = setup_server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }
    
    /* Run as daemon if requested */
    if (daemon_mode) {
        syslog(LOG_INFO, "Forking to run as daemon");
        if (run_as_daemon() == -1) {
            cleanup_resources();
            return EXIT_FAILURE;
        }
    }
    
    /* Initialize thread attributes for joinable threads */
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
    
    syslog(LOG_INFO, "Server listening on port %d (max %d concurrent connections)", 
           PORT, MAX_CONCURRENT_THREADS);
    
    /* Main server loop */
    while (!shutdown_requested) {
        client_len = sizeof(client_addr);
        
        /* Check if we can accept new connections */
        if (!can_accept_new_connection()) {
            /* Wait a bit before checking again */
            usleep(100000); /* 100ms */
            continue;
        }
        
        /* Accept incoming connection */
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd == -1) {
            if (errno == EINTR && shutdown_requested) {
                /* Interrupted by signal during shutdown */
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            }
            continue;
        }
        
        /* Get client IP address from accept() result */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        
        /* Allocate thread arguments */
        struct thread_args *thread_args = malloc(sizeof(struct thread_args));
        
        if (!thread_args) {
            syslog(LOG_ERR, "Failed to allocate thread arguments");
            close(client_fd);
            continue;
        }
        
        /* Create thread to handle connection */
        pthread_t thread_id;
        
        /* Set up thread arguments BEFORE creating thread */
        thread_args->client_fd = client_fd;
        strncpy(thread_args->client_ip, client_ip, sizeof(thread_args->client_ip) - 1);
        thread_args->client_ip[sizeof(thread_args->client_ip) - 1] = '\0';
        thread_args->thread_id = (uint64_t)thread_id; /* Will be updated after pthread_create */
        
        if (pthread_create(&thread_id, &thread_attr, connection_handler, thread_args) != 0) {
            syslog(LOG_ERR, "Failed to create thread for connection from %s: %s", 
                   client_ip, strerror(errno));
            free(thread_args);
            close(client_fd);
            continue;
        }
        
        /* Update thread ID in arguments - need to pass it back to thread */
        thread_args->thread_id = (uint64_t)thread_id;
        
        /* Register the thread */
        register_thread(thread_id, client_fd, client_ip, (uint64_t)thread_id);
    }
    
    /* Cleanup and exit */
    cleanup_resources();
    syslog(LOG_INFO, "Server shutdown complete");
    
    return EXIT_SUCCESS;
}
