#include "client.h"

// Convert random bytes to a hex string
int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size)
{
    if (buf == NULL || str == NULL || buf_size <= 0 ||
        str_size < (buf_size * 2 + 1))
    {
        return -1;
    }

    for (int i = 0; i < buf_size; i++)
        sprintf(str + i * 2, "%02X", buf[i]);
    str[buf_size * 2] = '\0';

    return 0;
}

// Initialize the client
void init_client(client_ctx_t *ctx, const char *server_ip, int server_port,
                 int num_messages, const char *log_file)
{
    ctx->server_ip = strdup(server_ip);
    ctx->server_port = server_port;
    ctx->num_messages = num_messages;
    ctx->log_file = strdup(log_file);
    ctx->socket_fd = -1;
    ctx->running = false;

    pthread_mutex_init(&ctx->mutex, NULL);
}

// Thread function to send messages to the server
void *send_messages(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int messages_sent = 0;

    while (ctx->running && messages_sent < ctx->num_messages)
    {
        // Generate random message
        uint8_t random_bytes[128];
        if (getentropy(random_bytes, sizeof(random_bytes)) != 0)
        {
            perror("Failed to get random bytes");
            break;
        }

        // Convert to hex string
        char hex_str[257]; // 256 + 1 for null terminator
        if (convert(random_bytes, sizeof(random_bytes), hex_str, sizeof(hex_str)) != 0)
        {
            fprintf(stderr, "Failed to convert random bytes to hex string\n");
            break;
        }

        // Create and send message
        char msg_buf[BUF_SIZE];
        int msg_len = snprintf(msg_buf, sizeof(msg_buf), "%c%s\n", 0, hex_str);

        if (msg_len < 0 || msg_len >= BUF_SIZE)
        {
            fprintf(stderr, "Message too long\n");
            break;
        }

        if (send(ctx->socket_fd, msg_buf, msg_len, 0) != msg_len)
        {
            perror("Failed to send message");
            break;
        }

        messages_sent++;

        // Sleep for a short time between messages
        usleep(50000); // 50ms
    }

    // Send type 1, end of sending message
    if (ctx->running)
    {
        char type1_msg[3] = {1, '\n', '\0'};
        send(ctx->socket_fd, type1_msg, 2, 0); // Send only 2 bytes (1 and newline)
    }

    return NULL;
}

// Thread function to receive messages from the server
void *receive_messages(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    FILE *log_file = fopen(ctx->log_file, "w");

    if (!log_file)
    {
        perror("Failed to open log file");
        ctx->running = false;
        return NULL;
    }

    char buffer[BUF_SIZE];
    int buffer_pos = 0;

    while (ctx->running)
    {
        // Read data from server
        ssize_t bytes_read = recv(ctx->socket_fd, buffer + buffer_pos, BUF_SIZE - buffer_pos - 1, 0);

        if (bytes_read <= 0)
        {
            if (bytes_read == 0)
            {
                printf("Server disconnected\n");
                break;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("Failed to read from server");
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
            // Make sure we have at least the type byte
            if (buffer_pos - processed_pos < 1)
            {
                break;
            }

            // Get message type
            uint8_t msg_type = (uint8_t)buffer[processed_pos];

            if (msg_type == 0)
            {
                // Regular message - ensure we have complete header (type + IP + port)
                int header_size = 1 + sizeof(uint32_t) + sizeof(uint16_t);
                if (buffer_pos - processed_pos < header_size)
                {
                    break; // Wait for more data
                }

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

                // Extract sender IP
                uint32_t sender_ip_net;
                memcpy(&sender_ip_net, buffer + processed_pos + 1, sizeof(sender_ip_net));
                uint32_t sender_ip = ntohl(sender_ip_net);

                // Extract sender port
                uint16_t sender_port_net;
                memcpy(&sender_port_net, buffer + processed_pos + 1 + sizeof(sender_ip_net), sizeof(sender_port_net));
                uint16_t sender_port = ntohs(sender_port_net);

                // Extract message content
                int content_size = msg_end - processed_pos - header_size;

                char msg_content[BUF_SIZE];
                if (content_size > 0 && content_size < BUF_SIZE)
                {
                    memcpy(msg_content, buffer + processed_pos + header_size, content_size);
                    msg_content[content_size] = '\0';
                }
                else
                {
                    msg_content[0] = '\0';
                }

                // Format IP address
                struct in_addr addr;
                addr.s_addr = htonl(sender_ip);
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));

                // Print and log message
                fprintf(log_file, "%-15s%-10u%s\n", ip_str, sender_port, msg_content);
                fflush(log_file);

                printf("%-15s%-10u%s\n", ip_str, sender_port, msg_content);

                // Move to next message
                processed_pos = msg_end + 1;
            }
            else if (msg_type == 1)
            {
                // Type 1 message (end of execution)
                printf("Received type 1 message from server. Terminating.\n");
                ctx->running = false;
                break;
            }
            else
            {
                // Unknown message type, skip to next byte
                processed_pos++;
            }
        }

        // Move any unprocessed data to the beginning of the buffer
        if (processed_pos > 0 && processed_pos < buffer_pos)
        {
            memmove(buffer, buffer + processed_pos, buffer_pos - processed_pos);
            buffer_pos -= processed_pos;
        }
        else if (processed_pos == buffer_pos)
        {
            buffer_pos = 0;
        }
    }

    fclose(log_file);
    return NULL;
}

// Run the client
void run_client(client_ctx_t *ctx)
{
    // Create socket
    ctx->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0)
    {
        perror("Failed to create socket");
        return;
    }

    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ctx->server_port);

    if (inet_pton(AF_INET, ctx->server_ip, &server_addr.sin_addr) <= 0)
    {
        perror("Invalid server IP address");
        close(ctx->socket_fd);
        return;
    }

    if (connect(ctx->socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Failed to connect to server");
        close(ctx->socket_fd);
        return;
    }

    // Set non-blocking mode
    int flags = fcntl(ctx->socket_fd, F_GETFL, 0);
    fcntl(ctx->socket_fd, F_SETFL, flags | O_NONBLOCK);

    // Start client
    ctx->running = true;

    // Start send and receive threads
    pthread_create(&ctx->send_thread, NULL, send_messages, ctx);
    pthread_create(&ctx->recv_thread, NULL, receive_messages, ctx);

    // Wait for threads to finish
    pthread_join(ctx->send_thread, NULL);
    pthread_join(ctx->recv_thread, NULL);
}

// Clean up client resources
void cleanup_client(client_ctx_t *ctx)
{
    // Close socket
    if (ctx->socket_fd >= 0)
    {
        close(ctx->socket_fd);
    }

    // Free resources
    free(ctx->server_ip);
    free(ctx->log_file);

    pthread_mutex_destroy(&ctx->mutex);
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <IP address> <port number> <# of messages> <log file path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int num_messages = atoi(argv[3]);
    const char *log_file = argv[4];

    if (server_port <= 0 || num_messages <= 0)
    {
        fprintf(stderr, "Invalid port number or message count\n");
        return EXIT_FAILURE;
    }

    client_ctx_t client;
    init_client(&client, server_ip, server_port, num_messages, log_file);
    run_client(&client);
    cleanup_client(&client);

    return EXIT_SUCCESS;
}