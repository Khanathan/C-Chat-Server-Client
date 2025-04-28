#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
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

#define BUF_SIZE 1024
#define LISTEN_BACKLOG 128

// Structure to store client information
typedef struct
{
    int fd;
    uint32_t ip;
    uint16_t port;
    bool sent_type_1;
} client_t;

// Structure for client list
typedef struct
{
    client_t *clients;
    int count;
    int max_clients;
    pthread_mutex_t mutex;
} client_list_t;

// Structure for messages
typedef struct
{
    uint8_t type;
    uint32_t sender_ip;
    uint16_t sender_port;
    char data[BUF_SIZE];
    int data_len;
} message_t;

// Structure for message queue node
typedef struct msg_node
{
    message_t msg;
    struct msg_node *next;
} msg_node_t;

// Structure for message queue
typedef struct
{
    msg_node_t *head;
    msg_node_t *tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} msg_queue_t;

// Structure for server context
typedef struct
{
    int server_fd;
    int port;
    int expected_clients;
    bool running;
    client_list_t clients;
    msg_queue_t msg_queue;
    pthread_mutex_t mutex;
    pthread_cond_t type_1_cond;
    int type_1_count;
} server_ctx_t;

// Structure to pass arguments to client handler
typedef struct
{
    server_ctx_t *ctx;
    int client_idx;
} client_handler_args_t;

// Global pthread key for thread-specific data
extern pthread_key_t server_key;

// Function prototypes
void init_server(server_ctx_t *ctx, int port, int expected_clients);
int add_client(server_ctx_t *ctx, int client_fd, uint32_t ip, uint16_t port);
void enqueue_message(msg_queue_t *queue, const message_t *msg);
void *client_handler(void *arg);
void *connection_acceptor(void *arg);
void *message_processor(void *arg);
void run_server(server_ctx_t *ctx);
void cleanup_server(server_ctx_t *ctx);

#endif // SERVER_H