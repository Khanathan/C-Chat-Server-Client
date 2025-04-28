#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

#define BUF_SIZE 1024

typedef struct
{
    char *server_ip;
    int server_port;
    int num_messages;
    char *log_file;
    int socket_fd;
    bool running;
    pthread_t send_thread;
    pthread_t recv_thread;
    pthread_mutex_t mutex;
} client_ctx_t;

// Function prototypes
int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size);
void init_client(client_ctx_t *ctx, const char *server_ip, int server_port,
                 int num_messages, const char *log_file);
void *send_messages(void *arg);
void *receive_messages(void *arg);
void run_client(client_ctx_t *ctx);
void cleanup_client(client_ctx_t *ctx);

#endif // CLIENT_H