/*********************************************************
 * Advanced Embedded Software Development - Spring 2026
 * Assignment 5 - Part 1 Program
 * 
 * Author  : Pranav Shastry
 * Email   : pranav.shastry@colorado.edu
 *
 * Date    : 15th February 2026
 *
 * AI Declaration:
 * All of my functions were reviewed by ChatGPT and I asked
 * it for recommendations on improvements or critical areas.
 * Some of the recommendations were incorporated to make the
 * program stronger.
 *
 * Chat Link : https://chatgpt.com/share/699292bd-16dc-8010-9f6f-7edf33c18f37
 *
 *********************************************************/


#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define SERVER_PORT 9000
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define RECV_CHUNK  1024
#define SEND_CHUNK  1024

static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;

static void handle_signal(int signo)
{
    (void)signo;
    g_exit_requested = 1;
}

static int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        return -1;
    }

    if (chdir("/") != 0) {
        return -1;
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        return -1;
    }
    (void)dup2(fd, STDIN_FILENO);
    (void)dup2(fd, STDOUT_FILENO);
    (void)dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    return 0;
}

static int create_listening_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
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

static int append_packet_to_file(const char *buf, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    close(fd);
    return 0;
}

static int send_file_to_client(int client_fd)
{
    int fd = open(DATA_FILE, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char out[SEND_CHUNK];
    while (1) {
        ssize_t r = read(fd, out, sizeof(out));
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;

        size_t off = 0;
        while (off < (size_t)r) {
            ssize_t s = send(client_fd, out + off, (size_t)r - off, 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            off += (size_t)s;
        }
    }

    close(fd);
    return 0;
}

static int handle_client(int client_fd)
{
    char *packet = NULL;
    size_t packet_len = 0;

    char buf[RECV_CHUNK];
    while (!g_exit_requested) {
        ssize_t r = recv(client_fd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(packet);
            return -1;
        }
        if (r == 0) {
            break;
        }

        char *newbuf = realloc(packet, packet_len + (size_t)r);
        if (!newbuf) {
            free(packet);
            return -1;
        }
        packet = newbuf;
        memcpy(packet + packet_len, buf, (size_t)r);
        packet_len += (size_t)r;

        size_t start = 0;
        for (size_t i = 0; i < packet_len; i++) {
            if (packet[i] == '\n') {
                size_t one_len = i - start + 1;

                if (append_packet_to_file(packet + start, one_len) != 0) {
                    free(packet);
                    return -1;
                }
                if (send_file_to_client(client_fd) != 0) {
                    free(packet);
                    return -1;
                }

                start = i + 1;
            }
        }

        if (start > 0) {
            size_t remain = packet_len - start;
            if (remain > 0) {
                memmove(packet, packet + start, remain);
            }
            packet_len = remain;

            char *shrink = realloc(packet, packet_len);
            if (shrink || packet_len == 0) {
                packet = shrink;
            }
        }
    }

    free(packet);
    return 0;
}

int main(int argc, char *argv[])
{
    bool run_as_daemon = false;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        run_as_daemon = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return EXIT_FAILURE;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGPIPE, SIG_IGN);


    if (setup_signal_handlers() != 0) {
        syslog(LOG_ERR, "Failed to set signal handlers: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    g_listen_fd = create_listening_socket();
    if (g_listen_fd < 0) {
        syslog(LOG_ERR, "Socket setup failed: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    if (run_as_daemon) {
        if (daemonize() != 0) {
            syslog(LOG_ERR, "Daemonize failed: %s", strerror(errno));
            close(g_listen_fd);
            g_listen_fd = -1;
            closelog();
            return EXIT_FAILURE;
        }
    }

    while (!g_exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || g_exit_requested) {
                break;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        char ipstr[INET_ADDRSTRLEN];
        memset(ipstr, 0, sizeof(ipstr));
        if (!inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr))) {
            strncpy(ipstr, "unknown", sizeof(ipstr) - 1);
        }

        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        (void)handle_client(client_fd);

        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);

        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    (void)unlink(DATA_FILE);

    closelog();
    return EXIT_SUCCESS;
}
