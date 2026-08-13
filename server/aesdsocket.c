#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../aesd-char-driver/aesd_ioctl.h"

#define PORT 9000
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#endif
#define BUFFER_SIZE 1024
#define TIMESTAMP_INTERVAL_SECONDS 10

typedef struct client_thread {
    pthread_t thread_id;
    int client_fd;
    char ip_address[INET_ADDRSTRLEN];
    bool complete;
    struct client_thread *next;
} client_thread_t;

static volatile sig_atomic_t exit_requested = 0;
static int server_fd = -1;

static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;
static client_thread_t *thread_list = NULL;

#if !USE_AESD_CHAR_DEVICE
static pthread_t timestamp_thread_id;
static bool timestamp_thread_started = false;
static bool timestamp_stop_requested = false;
static pthread_mutex_t timestamp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timestamp_condition = PTHREAD_COND_INITIALIZER;
#endif

static void signal_handler(int signal_number)
{
    (void)signal_number;
    exit_requested = 1;
}

static int daemonize_process(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    if (setsid() < 0 || chdir("/") != 0) {
        return -1;
    }

    umask(0);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    return 0;
}

static int send_all(int fd, const char *buffer, size_t length)
{
    size_t sent_total = 0;

    while (sent_total < length) {
        ssize_t sent = send(fd,
                            buffer + sent_total,
                            length - sent_total,
                            MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        sent_total += (size_t)sent;
    }

    return 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written_total = 0;

    while (written_total < length) {
        ssize_t written = write(fd,
                                buffer + written_total,
                                length - written_total);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        written_total += (size_t)written;
    }

    return 0;
}

/*
 * Receives one complete newline-terminated packet from a client.
 * Returns 1 for a complete packet, 0 if the peer closed before a complete
 * packet was received, and -1 on error.
 */
static int receive_packet(int client_fd, char **packet, size_t *packet_length)
{
    char buffer[BUFFER_SIZE];
    char *received_data = NULL;
    size_t used = 0;

    for (;;) {
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        char *newline;
        size_t copy_length;
        char *larger_buffer;

        if (bytes < 0) {
            if (errno == EINTR) {
                if (exit_requested) {
                    free(received_data);
                    return -1;
                }
                continue;
            }
            free(received_data);
            return -1;
        }
        if (bytes == 0) {
            free(received_data);
            return 0;
        }

        newline = memchr(buffer, '\n', (size_t)bytes);
        copy_length = newline != NULL
                          ? (size_t)(newline - buffer) + 1U
                          : (size_t)bytes;

        larger_buffer = realloc(received_data, used + copy_length);
        if (larger_buffer == NULL) {
            free(received_data);
            return -1;
        }

        received_data = larger_buffer;
        memcpy(received_data + used, buffer, copy_length);
        used += copy_length;

        if (newline != NULL) {
            *packet = received_data;
            *packet_length = used;
            return 1;
        }
    }
}

/*
 * Appends one complete client packet, then sends the complete data file back
 * to that client.  The same mutex protects both this operation and timestamp
 * writes so no two records can be intermixed.
 */

#if USE_AESD_CHAR_DEVICE
static bool parse_seek_command(const char *packet,
                               size_t packet_length,
                               struct aesd_seekto *seekto)
{
    static const char prefix[] = "AESDCHAR_IOCSEEKTO:";
    const size_t prefix_length = sizeof(prefix) - 1;
    const char *cursor;
    const char *end;
    uint64_t value;

    if (packet_length < prefix_length ||
        memcmp(packet, prefix, prefix_length) != 0) {
        return false;
    }

    cursor = packet + prefix_length;
    end = packet + packet_length;

    /*
     * Parse write_cmd.
     */
    if (cursor >= end || *cursor < '0' || *cursor > '9') {
        return false;
    }

    value = 0;

    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10 + (uint64_t)(*cursor - '0');

        if (value > UINT32_MAX) {
            return false;
        }

        cursor++;
    }

    if (cursor >= end || *cursor != ',') {
        return false;
    }

    seekto->write_cmd = (uint32_t)value;
    cursor++;

    /*
     * Parse write_cmd_offset.
     */
    if (cursor >= end || *cursor < '0' || *cursor > '9') {
        return false;
    }

    value = 0;

    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10 + (uint64_t)(*cursor - '0');

        if (value > UINT32_MAX) {
            return false;
        }

        cursor++;
    }

    seekto->write_cmd_offset = (uint32_t)value;

    /*
     * Socket packets normally include the terminating newline.
     */
    if (cursor < end && *cursor == '\n') {
        cursor++;
    }

    /*
     * Nothing else is allowed after X,Y except the newline.
     */
    return cursor == end;
}
#endif

static int append_and_return_file(int client_fd,
                                  const char *packet,
                                  size_t packet_length)
{
    int data_fd = -1;
    int result = -1;
    char buffer[BUFFER_SIZE];

#if USE_AESD_CHAR_DEVICE
    struct aesd_seekto seekto;
    bool is_seek_command;
#endif

    if (pthread_mutex_lock(&file_mutex) != 0) {
        return -1;
    }

#if USE_AESD_CHAR_DEVICE

    /*
     * Check whether this packet is the Assignment 9 seek command.
     */
    is_seek_command =
        parse_seek_command(packet, packet_length, &seekto);

    /*
     * Keep this descriptor open through the write/ioctl AND read.
     * This is required so ioctl changes to f_pos are preserved.
     */
    data_fd = open(DATA_FILE, O_RDWR);
    if (data_fd < 0) {
        goto cleanup;
    }

    if (is_seek_command) {
        /*
         * Do NOT write AESDCHAR_IOCSEEKTO:X,Y into the device.
         *
         * Instead, issue the ioctl.  This changes data_fd's
         * current file position.
         */
        if (ioctl(data_fd, AESDCHAR_IOCSEEKTO, &seekto) != 0) {
            goto cleanup;
        }
    } else {
        /*
         * Normal packet: write it to aesdchar.
         */
        if (write_all(data_fd, packet, packet_length) != 0) {
            goto cleanup;
        }

        /*
         * Normal socket writes return all current aesdchar
         * contents, so start reading from offset zero.
         */
        if (lseek(data_fd, 0, SEEK_SET) < 0) {
            goto cleanup;
        }
    }

#else

    /*
     * Original Assignment 6 file-backed implementation.
     */
    data_fd = open(DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (data_fd < 0 ||
        write_all(data_fd, packet, packet_length) != 0) {
        goto cleanup;
    }

    if (close(data_fd) != 0) {
        data_fd = -1;
        goto cleanup;
    }

    data_fd = -1;

    data_fd = open(DATA_FILE, O_RDONLY);
    if (data_fd < 0) {
        goto cleanup;
    }

#endif

    /*
     * For an ioctl request, this read uses the SAME file descriptor
     * on which ioctl changed f_pos.
     *
     * For a normal request, f_pos was explicitly reset to zero.
     */
    for (;;) {
        ssize_t bytes = read(data_fd, buffer, sizeof(buffer));

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }

            goto cleanup;
        }

        if (bytes == 0) {
            break;
        }

        if (send_all(client_fd, buffer, (size_t)bytes) != 0) {
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    if (data_fd >= 0) {
        close(data_fd);
    }

    pthread_mutex_unlock(&file_mutex);

    return result;
}

#if !USE_AESD_CHAR_DEVICE
static int append_timestamp(void)
{
    time_t now;
    struct tm local_time;
    char formatted_time[96];
    char timestamp_line[128];
    int timestamp_length;
    int data_fd = -1;
    int result = -1;

    now = time(NULL);
    if (now == (time_t)-1 || localtime_r(&now, &local_time) == NULL) {
        return -1;
    }

    if (strftime(formatted_time,
                 sizeof(formatted_time),
                 "%a, %d %b %Y %T %z",
                 &local_time) == 0) {
        return -1;
    }

    timestamp_length = snprintf(timestamp_line,
                                sizeof(timestamp_line),
                                "timestamp:%s\n",
                                formatted_time);
    if (timestamp_length < 0 ||
        (size_t)timestamp_length >= sizeof(timestamp_line)) {
        return -1;
    }

    if (pthread_mutex_lock(&file_mutex) != 0) {
        return -1;
    }

    data_fd = open(DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (data_fd >= 0 &&
        write_all(data_fd,
                  timestamp_line,
                  (size_t)timestamp_length) == 0) {
        result = 0;
    }

    if (data_fd >= 0) {
        close(data_fd);
    }
    pthread_mutex_unlock(&file_mutex);
    return result;
}

static void *timestamp_worker(void *argument)
{
    (void)argument;

    if (pthread_mutex_lock(&timestamp_mutex) != 0) {
        return NULL;
    }

    while (!timestamp_stop_requested) {
        struct timespec deadline;
        int wait_result = 0;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            break;
        }
        deadline.tv_sec += TIMESTAMP_INTERVAL_SECONDS;

        while (!timestamp_stop_requested && wait_result != ETIMEDOUT) {
            wait_result = pthread_cond_timedwait(&timestamp_condition,
                                                 &timestamp_mutex,
                                                 &deadline);
            if (wait_result != 0 && wait_result != ETIMEDOUT) {
                timestamp_stop_requested = true;
                break;
            }
        }

        if (timestamp_stop_requested) {
            break;
        }

        pthread_mutex_unlock(&timestamp_mutex);
        if (append_timestamp() != 0 && !exit_requested) {
            syslog(LOG_ERR, "Failed to append timestamp: %s", strerror(errno));
        }
        pthread_mutex_lock(&timestamp_mutex);
    }

    pthread_mutex_unlock(&timestamp_mutex);
    return NULL;
}

static int start_timestamp_thread(void)
{
    timestamp_stop_requested = false;

    if (pthread_create(&timestamp_thread_id,
                       NULL,
                       timestamp_worker,
                       NULL) != 0) {
        return -1;
    }

    timestamp_thread_started = true;
    return 0;
}

static void stop_timestamp_thread(void)
{
    if (!timestamp_thread_started) {
        return;
    }

    pthread_mutex_lock(&timestamp_mutex);
    timestamp_stop_requested = true;
    pthread_cond_signal(&timestamp_condition);
    pthread_mutex_unlock(&timestamp_mutex);

    pthread_join(timestamp_thread_id, NULL);
    timestamp_thread_started = false;
}

#endif

static void *client_worker(void *argument)
{
    client_thread_t *thread_data = argument;
    int client_fd = thread_data->client_fd;
    char *packet = NULL;
    size_t packet_length = 0;
    int receive_result;

    receive_result = receive_packet(client_fd, &packet, &packet_length);
    if (receive_result == 1) {
        if (append_and_return_file(client_fd, packet, packet_length) != 0 &&
            !exit_requested) {
            syslog(LOG_ERR,
                   "Failed to process connection from %s",
                   thread_data->ip_address);
        }
    } else if (receive_result < 0 && !exit_requested) {
        syslog(LOG_ERR,
               "Receive failed for %s: %s",
               thread_data->ip_address,
               strerror(errno));
    }

    free(packet);

    /* Prevent shutdown code from acting on a stale descriptor number. */
    pthread_mutex_lock(&list_mutex);
    thread_data->client_fd = -1;
    pthread_mutex_unlock(&list_mutex);

    close(client_fd);
    syslog(LOG_INFO, "Closed connection from %s", thread_data->ip_address);

    pthread_mutex_lock(&list_mutex);
    thread_data->complete = true;
    pthread_mutex_unlock(&list_mutex);

    return NULL;
}

static void add_thread(client_thread_t *thread_data)
{
    pthread_mutex_lock(&list_mutex);
    thread_data->next = thread_list;
    thread_list = thread_data;
    pthread_mutex_unlock(&list_mutex);
}

static void reap_completed_threads(void)
{
    for (;;) {
        client_thread_t **cursor;
        client_thread_t *completed = NULL;

        pthread_mutex_lock(&list_mutex);
        cursor = &thread_list;
        while (*cursor != NULL) {
            if ((*cursor)->complete) {
                completed = *cursor;
                *cursor = completed->next;
                break;
            }
            cursor = &(*cursor)->next;
        }
        pthread_mutex_unlock(&list_mutex);

        if (completed == NULL) {
            return;
        }

        pthread_join(completed->thread_id, NULL);
        free(completed);
    }
}

static void request_client_threads_exit(void)
{
    client_thread_t *current;

    pthread_mutex_lock(&list_mutex);
    for (current = thread_list; current != NULL; current = current->next) {
        if (current->client_fd >= 0) {
            shutdown(current->client_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&list_mutex);
}

static void join_all_client_threads(void)
{
    for (;;) {
        client_thread_t *current;

        pthread_mutex_lock(&list_mutex);
        current = thread_list;
        if (current != NULL) {
            thread_list = current->next;
        }
        pthread_mutex_unlock(&list_mutex);

        if (current == NULL) {
            break;
        }

        pthread_join(current->thread_id, NULL);
        free(current);
    }
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int result = EXIT_FAILURE;
    int option = 1;
    struct sockaddr_in server_address;
    struct sigaction action;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * Daemonize before creating any threads.  Only the child process should
     * own the timestamp thread and client worker threads.
     */
    if (daemon_mode && daemonize_process() != 0) {
        return EXIT_FAILURE;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

#if !USE_AESD_CHAR_DEVICE
    unlink(DATA_FILE);
#endif

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        goto cleanup;
    }

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &option,
                   sizeof(option)) != 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        goto cleanup;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        goto cleanup;
    }

    if (listen(server_fd, 10) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        goto cleanup;
    }

#if !USE_AESD_CHAR_DEVICE
    if (start_timestamp_thread() != 0) {
        syslog(LOG_ERR, "Failed to start timestamp thread");
        goto cleanup;
    }
#endif

    while (!exit_requested) {
        fd_set read_fds;
        struct timeval timeout = {1, 0};
        int select_result;

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        select_result = select(server_fd + 1,
                               &read_fds,
                               NULL,
                               NULL,
                               &timeout);

        reap_completed_threads();

        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "select failed: %s", strerror(errno));
            break;
        }
        if (select_result == 0) {
            continue;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in client_address;
            socklen_t client_length = sizeof(client_address);
            client_thread_t *thread_data;
            int client_fd;
            int create_result;

            client_fd = accept(server_fd,
                               (struct sockaddr *)&client_address,
                               &client_length);

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (!exit_requested) {
                    syslog(LOG_ERR, "accept failed: %s", strerror(errno));
                }
                continue;
            }

            thread_data = calloc(1, sizeof(*thread_data));
            if (thread_data == NULL) {
                close(client_fd);
                continue;
            }

            thread_data->client_fd = client_fd;
            if (inet_ntop(AF_INET,
                          &client_address.sin_addr,
                          thread_data->ip_address,
                          sizeof(thread_data->ip_address)) == NULL) {
                strcpy(thread_data->ip_address, "unknown");
            }

            syslog(LOG_INFO,
                   "Accepted connection from %s",
                   thread_data->ip_address);

            create_result = pthread_create(&thread_data->thread_id,
                                           NULL,
                                           client_worker,
                                           thread_data);
            if (create_result != 0) {
                syslog(LOG_ERR,
                       "pthread_create failed: %s",
                       strerror(create_result));
                close(client_fd);
                free(thread_data);
                continue;
            }

            add_thread(thread_data);
        }
    }

    result = EXIT_SUCCESS;

cleanup:
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    request_client_threads_exit();
#if !USE_AESD_CHAR_DEVICE
    stop_timestamp_thread();
#endif
    join_all_client_threads();
#if !USE_AESD_CHAR_DEVICE
    unlink(DATA_FILE);
#endif

    if (exit_requested) {
        syslog(LOG_INFO, "Caught signal, exiting");
    }

#if !USE_AESD_CHAR_DEVICE
    pthread_cond_destroy(&timestamp_condition);
    pthread_mutex_destroy(&timestamp_mutex);
#endif
    pthread_mutex_destroy(&list_mutex);
    pthread_mutex_destroy(&file_mutex);
    closelog();

    return result;
}
