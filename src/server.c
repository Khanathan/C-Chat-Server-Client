// #include "server.h"
#include "../include/server.h"

// Global pthread key for thread-specific data
pthread_key_t server_key;

int make_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("Failed to get socket flags");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("Failed to set socket non-blocking");
        return -1;
    }
    return 0;
}

// Initialize a new server context
void init_server(server_ctx_t *ctx, int port, int expected_clients)
{
    // Initialize server context
    ctx->port = port;
    ctx->expected_clients = expected_clients;
    ctx->running = true;
    ctx->type_1_count = 0;

    // Initialize pthread key for thread-specific data
    pthread_key_create(&server_key, NULL);

    // Initialize mutexes and condition variables
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->type_1_cond, NULL);

    // Initialize client list
    ctx->clients.clients = malloc(expected_clients * sizeof(client_t));
    if (ctx->clients.clients == NULL)
    {
        perror("Failed to allocate client list");
        exit(EXIT_FAILURE);
    }

    ctx->clients.max_clients = expected_clients;
    ctx->clients.count = 0;
    pthread_mutex_init(&ctx->clients.mutex, NULL);

    // Initialize message queue
    ctx->msg_queue.head = NULL;
    ctx->msg_queue.tail = NULL;
    ctx->msg_queue.count = 0;
    pthread_mutex_init(&ctx->msg_queue.mutex, NULL);
    pthread_cond_init(&ctx->msg_queue.cond, NULL);

    // Create server socket
    ctx->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->server_fd < 0)
    {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Make socket non-blocking
    if (make_socket_nonblocking(ctx->server_fd) < 0)
    {
        perror("Failed to make socket non-blocking");
        close(ctx->server_fd);
        exit(EXIT_FAILURE);
    }

    // Set socket options to allow reuse of local addresses
    int opt = 1;
    if (setsockopt(ctx->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("Failed to set socket options");
        exit(EXIT_FAILURE);
    }

    // Bind socket to port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(ctx->server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Failed to bind socket");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(ctx->server_fd, LISTEN_BACKLOG) < 0)
    {
        perror("Failed to listen on socket");
        exit(EXIT_FAILURE);
    }

    printf("Server started on port %d\n", port);
}

// Add a client to the client list
int add_client(server_ctx_t *ctx, int client_fd, uint32_t ip, uint16_t port)
{
    pthread_mutex_lock(&ctx->clients.mutex);

    if (ctx->clients.count >= ctx->clients.max_clients)
    {
        pthread_mutex_unlock(&ctx->clients.mutex);
        return -1;
    }

    int idx = ctx->clients.count;
    ctx->clients.clients[idx].fd = client_fd;
    ctx->clients.clients[idx].ip = ip;
    ctx->clients.clients[idx].port = port;
    ctx->clients.clients[idx].sent_type_1 = false;
    ctx->clients.count++;

    pthread_mutex_unlock(&ctx->clients.mutex);
    return idx;
}

// Add a message to the message queue
void enqueue_message(msg_queue_t *queue, const message_t *msg)
{
    msg_node_t *node = malloc(sizeof(msg_node_t));
    if (!node)
    {
        perror("Failed to allocate message node");
        exit(EXIT_FAILURE);
    }

    memcpy(&node->msg, msg, sizeof(message_t));
    node->next = NULL;

    pthread_mutex_lock(&queue->mutex);

    if (queue->tail)
    {
        queue->tail->next = node;
    }
    else
    {
        queue->head = node;
    }

    queue->tail = node;
    queue->count++;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

// Thread function to handle client connections
void *client_handler(void *arg)
{
    client_handler_args_t *handler_args = (client_handler_args_t *)arg;
    server_ctx_t *ctx = handler_args->ctx;
    int client_idx = handler_args->client_idx;
    free(arg);

    client_t *client = NULL;
    int client_fd = -1;

    // Access client safely
    pthread_mutex_lock(&ctx->clients.mutex);
    if (client_idx >= 0 && client_idx < ctx->clients.count)
    {
        client = &ctx->clients.clients[client_idx];
        client_fd = client->fd;
    }
    pthread_mutex_unlock(&ctx->clients.mutex);

    if (client_fd < 0)
    {
        fprintf(stderr, "Invalid client index: %d\n", client_idx);
        return NULL;
    }

    char buffer[BUF_SIZE];
    int buffer_pos = 0;

    while (ctx->running)
    {
        // Read data from client
        ssize_t bytes_read = read(client_fd, buffer + buffer_pos, BUF_SIZE - buffer_pos - 1);

        if (bytes_read <= 0)
        {
            if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                // Client disconnected or error occurred
                break;
            }
            continue;
        }

        buffer_pos += bytes_read;
        buffer[buffer_pos] = '\0'; // Ensure null termination

        // Process complete messages
        int processed_pos = 0;
        while (processed_pos < buffer_pos)
        {
            // Find end of message (newline character)
            int msg_end = processed_pos;
            while (msg_end < buffer_pos && buffer[msg_end] != '\n')
            {
                msg_end++;
            }

            if (msg_end >= buffer_pos)
            {
                // No complete message found, wait for more data
                break;
            }

            // Process message
            message_t msg;
            msg.type = buffer[processed_pos];

            // Get client IP and port safely
            pthread_mutex_lock(&ctx->clients.mutex);
            if (client_idx >= 0 && client_idx < ctx->clients.count)
            {
                msg.sender_ip = ctx->clients.clients[client_idx].ip;
                msg.sender_port = ctx->clients.clients[client_idx].port;
            }
            else
            {
                msg.sender_ip = 0;
                msg.sender_port = 0;
            }
            pthread_mutex_unlock(&ctx->clients.mutex);

            int msg_len = msg_end - processed_pos;
            if (msg_len > 1)
            {
                msg.data_len = msg_len - 1;
                if (msg.data_len >= BUF_SIZE)
                    msg.data_len = BUF_SIZE - 1;
                memcpy(msg.data, buffer + processed_pos + 1, msg.data_len);
                msg.data[msg.data_len] = '\0';
            }
            else
            {
                msg.data[0] = '\0';
                msg.data_len = 0;
            }

            // Handle message based on type
            if (msg.type == 1)
            {
                // Type 1 message (client done sending)
                pthread_mutex_lock(&ctx->mutex);
                pthread_mutex_lock(&ctx->clients.mutex);
                if (client_idx >= 0 && client_idx < ctx->clients.count)
                {
                    ctx->clients.clients[client_idx].sent_type_1 = true;
                    ctx->type_1_count++;
                }
                pthread_mutex_unlock(&ctx->clients.mutex);
                pthread_cond_signal(&ctx->type_1_cond);
                pthread_mutex_unlock(&ctx->mutex);
            }

            // Add message to queue
            enqueue_message(&ctx->msg_queue, &msg);

            // Move to next message
            processed_pos = msg_end + 1;
        }

        // Move any unprocessed data to the beginning of the buffer
        if (processed_pos < buffer_pos)
        {
            memmove(buffer, buffer + processed_pos, buffer_pos - processed_pos);
        }
        buffer_pos -= processed_pos;
    }

    close(client_fd);
    return NULL;
}

// Thread function to accept client connections
void *connection_acceptor(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    pthread_setspecific(server_key, ctx);

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (ctx->running)
    {
        // Accept a client connection
        int client_fd = accept(ctx->server_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            perror("Failed to accept client connection");
            break;
        }

        // Set non-blocking mode
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // Get client IP and port
        uint32_t client_ip = ntohl(client_addr.sin_addr.s_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);

        // Add client to list
        int client_idx = add_client(ctx, client_fd, client_ip, client_port);
        if (client_idx < 0)
        {
            close(client_fd);
            continue;
        }

        // Start client handler thread
        client_handler_args_t *args = malloc(sizeof(client_handler_args_t));
        if (!args)
        {
            perror("Failed to allocate memory for client handler arguments");
            close(client_fd);
            continue;
        }

        args->ctx = ctx;
        args->client_idx = client_idx;

        pthread_t thread;
        pthread_create(&thread, NULL, client_handler, args);
        pthread_detach(thread);
    }

    return NULL;
}

// Thread function to process and relay messages
void *message_processor(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    msg_queue_t *queue = &ctx->msg_queue;

    while (ctx->running)
    {
        // Wait for messages
        pthread_mutex_lock(&queue->mutex);
        while (queue->head == NULL && ctx->running)
        {
            pthread_cond_wait(&queue->cond, &queue->mutex);
        }

        if (!ctx->running)
        {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        // Get message from queue
        msg_node_t *node = queue->head;
        queue->head = node->next;

        if (queue->head == NULL)
        {
            queue->tail = NULL;
        }

        queue->count--;
        pthread_mutex_unlock(&queue->mutex);

        message_t *msg = &node->msg;

        // Process message
        if (msg->type == 0)
        {
            // Type 0 message (regular message)
            // Construct the message to send to all clients
            char send_buf[BUF_SIZE];
            int send_pos = 0;

            // Type
            send_buf[send_pos++] = 0;

            // Sender IP address (in network byte order)
            uint32_t ip_net = htonl(msg->sender_ip);
            memcpy(send_buf + send_pos, &ip_net, sizeof(ip_net));
            send_pos += sizeof(ip_net);

            // Sender port (in network byte order)
            uint16_t port_net = htons(msg->sender_port);
            memcpy(send_buf + send_pos, &port_net, sizeof(port_net));
            send_pos += sizeof(port_net);

            // Message data
            if (msg->data_len > 0 && msg->data_len < BUF_SIZE - send_pos - 1)
            {
                memcpy(send_buf + send_pos, msg->data, msg->data_len);
                send_pos += msg->data_len;
            }

            // Newline
            send_buf[send_pos++] = '\n';

            // Send to all clients
            pthread_mutex_lock(&ctx->clients.mutex);
            for (int i = 0; i < ctx->clients.count; i++)
            {
                if (ctx->clients.clients[i].fd > 0)
                {
                    send(ctx->clients.clients[i].fd, send_buf, send_pos, 0);
                }
            }
            pthread_mutex_unlock(&ctx->clients.mutex);
        }

        free(node);
    }

    return NULL;
}

// Run the server
void run_server(server_ctx_t *ctx)
{
    // Start connection acceptor thread
    pthread_t acceptor_thread;
    pthread_create(&acceptor_thread, NULL, connection_acceptor, ctx);

    // Start message processor thread
    pthread_t processor_thread;
    pthread_create(&processor_thread, NULL, message_processor, ctx);

    // Wait for all clients to send type 1 messages
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->type_1_count < ctx->clients.count || ctx->clients.count < ctx->expected_clients)
    {
        pthread_cond_wait(&ctx->type_1_cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);

    printf("All clients have sent type 1 messages. Shutting down.\n");

    // Send type 1 message to all clients
    char type_1_msg[2] = {1, '\n'};
    pthread_mutex_lock(&ctx->clients.mutex);
    for (int i = 0; i < ctx->clients.count; i++)
    {
        if (ctx->clients.clients[i].fd > 0)
        {
            send(ctx->clients.clients[i].fd, type_1_msg, sizeof(type_1_msg), 0);
        }
    }
    pthread_mutex_unlock(&ctx->clients.mutex);

    // Shutdown server
    ctx->running = false;
    pthread_cond_signal(&ctx->msg_queue.cond);

    // Wait for threads to finish
    pthread_join(acceptor_thread, NULL);
    pthread_join(processor_thread, NULL);
}

// Clean up server resources
void cleanup_server(server_ctx_t *ctx)
{
    // Close server socket
    close(ctx->server_fd);

    // Free client list
    free(ctx->clients.clients);
    pthread_mutex_destroy(&ctx->clients.mutex);

    // Free message queue
    msg_node_t *node = ctx->msg_queue.head;
    while (node)
    {
        msg_node_t *next = node->next;
        free(node);
        node = next;
    }

    pthread_mutex_destroy(&ctx->msg_queue.mutex);
    pthread_cond_destroy(&ctx->msg_queue.cond);

    // Destroy mutexes and condition variables
    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->type_1_cond);

    // Delete pthread key
    pthread_key_delete(server_key);
}

#ifndef UNIT_TEST
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <port number> <# of clients>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    int expected_clients = atoi(argv[2]);

    if (port <= 0 || expected_clients <= 0)
    {
        fprintf(stderr, "Invalid port number or client count\n");
        return EXIT_FAILURE;
    }

    server_ctx_t server;
    init_server(&server, port, expected_clients);
    run_server(&server);
    cleanup_server(&server);

    return EXIT_SUCCESS;
}
#endif