/*********************************************************
 * Advanced Embedded Software Development - Spring 2026
 * Assignment 6 - Part 1 Program (Threading + Timestamps)
 *
 * Based on Assignment 5 Part 1 Program by Pranav Shastry
 *
 * Key changes for Assignment 6:
 *  - Thread-per-connection model (pthread_create after accept)
 *  - Thread tracking via singly-linked list (SLIST from sys/queue.h)
 *  - Join completed threads (pthread_join) in main thread
 *  - Mutex protection for /var/tmp/aesdsocketdata accesses
 *  - Timestamp line appended every 10 seconds from a timestamp thread
 *  - Graceful shutdown on SIGINT/SIGTERM:
 *      * Stop accept()
 *      * Shutdown client sockets to unblock recv()
 *      * Join all threads, then exit cleanly
 *
 * AI Declaration:
 * ChatGPT was used for debugging certain snippets and
 * also adding comments into the code. Assignment 5 aesdsocket.c
 * file was given as reference and I asked for inputs on 
 * how I should go about on coding for assignment 6. This was
 * then given for inspection and certain suggestions which it
 * provided were incorporated into the code.
 *
 * Chat History: https://chatgpt.com/share/699bcda2-01a0-8010-a238-d46d0b7094c1
 *********************************************************/

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define RECV_CHUNK 1024
#define SEND_CHUNK 1024
#define TIMESTAMP_PERIOD_SEC 10

/*
 * Global shutdown flag set by signal handler.
 * - sig_atomic_t is used because it is safe to modify in a signal handler.
 * - volatile ensures the compiler does not optimize reads/writes away.
 */
static volatile sig_atomic_t g_exit_requested = 0;

/*
 * Listening socket FD stored globally so signal handler can close it.
 * Closing the listen socket unblocks accept() so the main thread can exit.
 */
static int g_listen_fd = -1;

/*
 * Mutex to protect access to the shared data file.
 * - Without this, multiple threads could write at the same time and interleave bytes.
 * - We also lock around reading/sending the file to avoid reading while another thread writes.
 */
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Timestamp thread handle so we can join it at shutdown.
 */
static pthread_t g_timestamp_thread;

/*
 * Per-connection thread tracking node.
 * This is what we store in the SLIST.
 */
struct thread_node {
    pthread_t thread_id;             // pthread handle for this client connection thread
    int client_fd;                   // connected client socket FD
    bool thread_complete;            // set true by worker thread before it exits
    char client_ip[INET_ADDRSTRLEN]; // optional: store client IP (currently unused)
    SLIST_ENTRY(thread_node) entries;
};

/*
 * Define and create the singly linked list head for thread tracking.
 * You must SLIST_INIT(&g_thread_list) before use.
 */
SLIST_HEAD(thread_list_head, thread_node);
static struct thread_list_head g_thread_list;

/*
 * Utility wrapper: close() the fd only if it is valid,
 * and then set it to -1 to avoid accidental double-close.
 */
static void safe_close(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/*
 * Signal handler: keep it VERY small and async-signal-safe.
 * We do only:
 *  - set shutdown flag
 *  - close listening socket to unblock accept()
 *
 * NOTE: close() is async-signal-safe. Avoid syslog(), malloc(), printf(), etc here.
 */
static void handle_signal(int signo)
{
    (void)signo;
    g_exit_requested = 1;

    /*
     * Close the listening socket so accept() in main thread returns.
     * After this, main loop will break and proceed to clean shutdown.
     */
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/*
 * Install SIGINT and SIGTERM handlers.
 * These signals should cause a graceful shutdown.
 */
static int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;

    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

/*
 * Daemonize:
 * - fork and exit parent
 * - setsid to detach from terminal
 * - chdir("/") to avoid locking directories
 * - redirect stdin/stdout/stderr to /dev/null
 */
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS); // parent exits

    if (setsid() < 0) return -1;
    if (chdir("/") != 0) return -1;

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return -1;

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);

    return 0;
}

/*
 * Create a TCP listening socket bound to port 9000 on all interfaces.
 */
static int create_listening_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /*
     * SO_REUSEADDR allows re-binding quickly after restart
     * (e.g., if the port is in TIME_WAIT).
     */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Append a byte buffer to the shared data file.
 * Mutex-protected to prevent interleaving between threads.
 */
static int append_packet_to_file(const char *buf, size_t len)
{
    int rc = 0;

    /*
     * Only one thread at a time can open+append+write the shared file.
     */
    pthread_mutex_lock(&g_file_mutex);

    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        pthread_mutex_unlock(&g_file_mutex);
        return -1;
    }

    /*
     * Handle partial writes: loop until everything is written.
     */
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }

    close(fd);
    pthread_mutex_unlock(&g_file_mutex);
    return rc;
}

/*
 * Read the entire data file and send it to the client.
 * We also lock the same mutex here to prevent reading while another thread writes.
 *
 * NOTE: This means while one client is being served, timestamp and other clients
 * may block briefly. This is acceptable for this assignment and preserves the
 * "append then return entire file" behavior consistently.
 */
static int send_file_to_client(int client_fd)
{
    pthread_mutex_lock(&g_file_mutex);

    int fd = open(DATA_FILE, O_RDONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&g_file_mutex);
        return 0;
    }

    char out[SEND_CHUNK];

    while (1) {
        /*
         * Read file in chunks and send to socket.
         */
        ssize_t r = read(fd, out, sizeof(out));
        if (r <= 0) break;

        /*
         * send() may send fewer bytes than requested; loop until complete.
         */
        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t s = send(client_fd, out + off, (size_t)r - off, 0);
            if (s <= 0) break;  // client disconnected or error
            off += (size_t)s;
        }
    }

    close(fd);
    pthread_mutex_unlock(&g_file_mutex);
    return 0;
}

/*
 * handle_client:
 * - Receives data from a single client socket
 * - Buffers received bytes until it finds newline '\n'
 * - For each newline-terminated packet:
 *      1) append to file
 *      2) send the full file back to the client
 *
 * This function is called by each client connection thread.
 */
static int handle_client(int client_fd)
{
    char *packet = NULL;      // dynamic buffer holding partially received line(s)
    size_t packet_len = 0;    // current valid bytes in 'packet'
    char buf[RECV_CHUNK];     // temporary recv buffer

    /*
     * Loop until client disconnects or server shutdown requested.
     */
    while (!g_exit_requested) {
        ssize_t r = recv(client_fd, buf, sizeof(buf), 0);
        if (r <= 0) break; // 0 = orderly shutdown by peer, <0 = error

        /*
         * Grow packet buffer and append newly received bytes.
         */
        char *newbuf = realloc(packet, packet_len + (size_t)r);
        if (!newbuf) {
            free(packet);
            return -1;
        }

        packet = newbuf;
        memcpy(packet + packet_len, buf, (size_t)r);
        packet_len += (size_t)r;

        /*
         * Scan packet buffer for '\n' characters.
         * Every time we find one, we have a complete packet to process.
         */
        size_t start = 0;
        for (size_t i = 0; i < packet_len; i++) {
            if (packet[i] == '\n') {
                size_t one_len = i - start + 1; // include newline

                /*
                 * Append only the complete packet portion to the file.
                 */
                if (append_packet_to_file(packet + start, one_len) != 0) {
                    free(packet);
                    return -1;
                }

                /*
                 * Then send the whole file contents back to the client.
                 * This mimics the Assignment 5 behavior.
                 */
                if (send_file_to_client(client_fd) != 0) {
                    free(packet);
                    return -1;
                }

                start = i + 1; // next packet begins after '\n'
            }
        }

        /*
         * If we consumed some bytes (processed one or more packets),
         * shift the remaining partial packet to the front of the buffer.
         */
        if (start > 0) {
            size_t remain = packet_len - start;
            memmove(packet, packet + start, remain);
            packet_len = remain;
        }
    }

    free(packet);
    return 0;
}

/*
 * Timestamp thread:
 * Every 10 seconds, append a line of the form:
 *   timestamp:<RFC2822-time>\n
 *
 * It exits promptly when g_exit_requested is set.
 */
static void *timestamp_thread_func(void *arg)
{
    (void)arg;

    while (!g_exit_requested) {
        /*
         * Sleep in 1-second increments so we can respond quickly to shutdown.
         */
        for (int i = 0; i < TIMESTAMP_PERIOD_SEC && !g_exit_requested; i++) {
            sleep(1);
        }
        if (g_exit_requested) break;

        /*
         * Get current local time and format it.
         * RFC 2822 style like: "Sun, 22 Feb 2026 19:05:00 -0700"
         */
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char timebuf[128];
        if (strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S %z", &tm_now) == 0) {
            continue; // formatting failed
        }

        char line[256];
        int n = snprintf(line, sizeof(line), "timestamp:%s\n", timebuf);
        if (n > 0) {
            /*
             * Reuse same append function; it already locks the file mutex.
             */
            (void)append_packet_to_file(line, (size_t)n);
        }
    }

    return NULL;
}

/*
 * Optional argument wrapper for connection thread.
 * This avoids passing multiple parameters; we pass a single pointer.
 */
struct conn_thread_args {
    struct thread_node *node;
};

/*
 * Per-connection thread entry:
 * - Processes the client socket with handle_client()
 * - Closes client socket
 * - Marks its node complete so main can join() and free memory
 */
static void *connection_thread_func(void *arg)
{
    struct conn_thread_args *args = arg;
    struct thread_node *node = args->node;
    free(args); // free thread args immediately

    (void)handle_client(node->client_fd);

    /*
     * Shutdown and close the socket:
     * - shutdown() helps terminate recv()/send() gracefully
     * - safe_close() prevents double-close
     */
    shutdown(node->client_fd, SHUT_RDWR);
    safe_close(&node->client_fd);

    /*
     * Signal to main thread that join() is now safe.
     * Main thread will join and free this node.
     */
    node->thread_complete = true;
    return NULL;
}

/*
 * Helper: join and remove any completed client threads from the SLIST.
 * We must use SLIST macros (portable), not manual pointer assignment.
 */
static void join_completed_threads(void)
{
    struct thread_node *prev = NULL;
    struct thread_node *cur = SLIST_FIRST(&g_thread_list);

    while (cur != NULL) {
        struct thread_node *next = SLIST_NEXT(cur, entries);

        if (cur->thread_complete) {
            pthread_join(cur->thread_id, NULL);

            if (prev == NULL) {
                SLIST_REMOVE_HEAD(&g_thread_list, entries);
            } else {
                /* Portable equivalent of "remove after prev" */
                SLIST_NEXT(prev, entries) = next;
            }

            free(cur);
            /* prev stays the same */
        } else {
            prev = cur;
        }

        cur = next;
    }
}


int main(int argc, char *argv[])
{
    bool run_as_daemon = false;

    /*
     * Parse command-line argument:
     * - no args: run in foreground
     * - -d: run as daemon
     */
    if (argc == 2 && strcmp(argv[1], "-d") == 0)
        run_as_daemon = true;
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return EXIT_FAILURE;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    /*
     * Avoid terminating the process if client disconnects while we send().
     * Instead send() fails with EPIPE and we handle it.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Initialize thread list head before inserting nodes.
     */
    SLIST_INIT(&g_thread_list);

    /*
     * Install SIGINT/SIGTERM handlers.
     */
    if (setup_signal_handlers() != 0)
        return EXIT_FAILURE;

    /*
     * Create and bind listen socket before daemonize (either is fine,
     * but doing it before daemonize keeps failure messages visible).
     */
    g_listen_fd = create_listening_socket();
    if (g_listen_fd < 0)
        return EXIT_FAILURE;

    /*
     * Optionally daemonize.
     */
    if (run_as_daemon && daemonize() != 0)
        return EXIT_FAILURE;

    /*
     * Start timestamp thread. It runs until g_exit_requested becomes true.
     */
    if (pthread_create(&g_timestamp_thread, NULL, timestamp_thread_func, NULL) != 0)
        return EXIT_FAILURE;

    /*
     * Main accept loop:
     * - accept new client connections
     * - create a worker thread for each connection
     * - insert thread node into SLIST
     * - periodically join any completed threads
     */
    while (!g_exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /*
         * accept() blocks waiting for a connection.
         * When SIGINT/SIGTERM occurs, our handler closes g_listen_fd,
         * which causes accept() to fail and we break out.
         */
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_exit_requested) break;
            continue; // could also check errno == EINTR
        }

        /*
         * Allocate thread tracking node for this connection.
         * This node lives until main joins the thread and frees it.
         */
        struct thread_node *node = calloc(1, sizeof(*node));
        if (!node) {
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->thread_complete = false;

        /*
         * Allocate wrapper args object to pass node pointer to thread.
         * (Could also just pass node directly to thread func and skip args struct.)
         */
        struct conn_thread_args *args = calloc(1, sizeof(*args));
        if (!args) {
            close(client_fd);
            free(node);
            continue;
        }
        args->node = node;

        /*
         * Create worker thread for this client.
         */
        if (pthread_create(&node->thread_id, NULL, connection_thread_func, args) != 0) {
            close(client_fd);
            free(args);
            free(node);
            continue;
        }

        /*
         * Insert node in thread tracking list.
         * Main uses this list later to shutdown sockets and join threads.
         */
        SLIST_INSERT_HEAD(&g_thread_list, node, entries);

        /*
         * Opportunistic cleanup: join threads that already finished.
         * This prevents list growth if many short-lived clients connect.
         */
        join_completed_threads();
    }

    /*
     * Shutdown phase:
     * 1) close listening socket (already closed by signal handler sometimes)
     * 2) shutdown all client sockets to unblock recv() in worker threads
     * 3) join all worker threads and free nodes
     * 4) join timestamp thread
     * 5) delete file and destroy mutex
     */
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        safe_close(&g_listen_fd);
    }

    /*
     * Ask all client threads to exit quickly by shutting down their sockets.
     * recv() should return and handle_client() can exit.
     */
    struct thread_node *it = NULL;
    SLIST_FOREACH(it, &g_thread_list, entries) {
        if (it->client_fd >= 0) {
            shutdown(it->client_fd, SHUT_RDWR);
        }
    }

    /*
     * Join all remaining client threads and free their nodes.
     */
    while (!SLIST_EMPTY(&g_thread_list)) {
        it = SLIST_FIRST(&g_thread_list);
        pthread_join(it->thread_id, NULL);
        SLIST_REMOVE_HEAD(&g_thread_list, entries);
        free(it);
    }

    /*
     * Join timestamp thread last.
     * It will exit once g_exit_requested is set.
     */
    pthread_join(g_timestamp_thread, NULL);

    /*
     * Cleanup resources.
     */
    unlink(DATA_FILE);
    pthread_mutex_destroy(&g_file_mutex);
    closelog();

    return EXIT_SUCCESS;
}
