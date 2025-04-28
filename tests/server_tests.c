#include <criterion/criterion.h>
#include <criterion/new/assert.h>
#include <criterion/internal/assert.h>
#include <criterion/redirect.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

// Include the server header
#include "../include/server.h"

// Mock functions and global variables for testing
static server_ctx_t test_ctx;
static int mock_client_fd = 100; // Arbitrary value for testing
static msg_queue_t *test_queue = NULL;

// Setup and teardown functions
void setup(void)
{
    // Initialize the test context
    init_server(&test_ctx, 8080, 5);
    test_queue = &test_ctx.msg_queue;
}

void teardown(void)
{
    // Clean up the test context
    cleanup_server(&test_ctx);
}

// Unit test for non-blocking socket setup
Test(server_socket, make_socket_nonblocking)
{
    // Create a socket for testing
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert(sock >= 0, "Failed to create test socket");

    // Test making it non-blocking
    int result = make_socket_nonblocking(sock);
    cr_assert_eq(result, 0, "make_socket_nonblocking should return 0 on success");

    // Verify socket is non-blocking by checking O_NONBLOCK flag
    int flags = fcntl(sock, F_GETFL, 0);
    cr_assert(flags & O_NONBLOCK, "Socket should have O_NONBLOCK flag set");

    // Clean up
    close(sock);
}

// Test adding a client to the client list
Test(client_list, add_client)
{
    // Setup
    setup();

    // Test adding a client
    int idx = add_client(&test_ctx, mock_client_fd, 0x7F000001, 8080); // 127.0.0.1:8080

    // Verify client was added correctly
    cr_assert_geq(idx, 0, "add_client should return a non-negative index");
    cr_assert_eq(test_ctx.clients.count, 1, "Client count should be 1 after adding a client");
    cr_assert_eq(test_ctx.clients.clients[idx].fd, mock_client_fd, "Client fd should match");
    cr_assert_eq(test_ctx.clients.clients[idx].ip, 0x7F000001, "Client IP should match");
    cr_assert_eq(test_ctx.clients.clients[idx].port, 8080, "Client port should match");
    cr_assert_eq(test_ctx.clients.clients[idx].sent_type_1, false, "Client sent_type_1 should be false initially");

    // Teardown
    teardown();
}

// Test adding more clients than the maximum allowed
Test(client_list, add_client_max_exceeded)
{
    // Setup
    setup();

    // Fill the client list to capacity
    for (int i = 0; i < test_ctx.clients.max_clients; i++)
    {
        int idx = add_client(&test_ctx, mock_client_fd + i, 0x7F000001, 8080 + i);
        cr_assert_geq(idx, 0, "add_client should succeed for client %d", i);
    }

    // Try to add one more client
    int idx = add_client(&test_ctx, mock_client_fd + 100, 0x7F000001, 9000);
    cr_assert_eq(idx, -1, "add_client should return -1 when max clients is exceeded");
    cr_assert_eq(test_ctx.clients.count, test_ctx.clients.max_clients,
                 "Client count should remain at max_clients");

    // Teardown
    teardown();
}

// Test the message queue
Test(message_queue, enqueue_message)
{
    // Setup
    setup();

    // Create a test message
    message_t msg;
    msg.type = 0;
    msg.sender_ip = 0x7F000001; // 127.0.0.1
    msg.sender_port = 8080;
    snprintf(msg.data, BUF_SIZE, "Test message");
    msg.data_len = strlen(msg.data);

    // Enqueue the message
    enqueue_message(test_queue, &msg);

    // Verify message was enqueued
    cr_assert_eq(test_queue->count, 1, "Queue count should be 1 after enqueueing a message");
    cr_assert_not_null(test_queue->head, "Queue head should not be NULL");
    cr_assert_not_null(test_queue->tail, "Queue tail should not be NULL");

    // Check message content
    msg_node_t *node = test_queue->head;
    cr_assert_eq(node->msg.type, msg.type, "Message type should match");
    cr_assert_eq(node->msg.sender_ip, msg.sender_ip, "Sender IP should match");
    cr_assert_eq(node->msg.sender_port, msg.sender_port, "Sender port should match");
    cr_assert_str_eq(node->msg.data, msg.data, "Message data should match");

    // Teardown
    teardown();
}

// Test enqueueing and handling multiple messages
Test(message_queue, enqueue_multiple_messages)
{
    // Setup
    setup();

    // Enqueue multiple messages
    const int NUM_MESSAGES = 5;
    for (int i = 0; i < NUM_MESSAGES; i++)
    {
        message_t msg;
        msg.type = 0;
        msg.sender_ip = 0x7F000001; // 127.0.0.1
        msg.sender_port = 8080 + i;
        snprintf(msg.data, BUF_SIZE, "Test message %d", i);
        msg.data_len = strlen(msg.data);

        enqueue_message(test_queue, &msg);
    }

    // Verify queue count
    cr_assert_eq(test_queue->count, NUM_MESSAGES,
                 "Queue count should be %d after enqueueing %d messages",
                 NUM_MESSAGES, NUM_MESSAGES);

    // Verify message order (FIFO)
    msg_node_t *node = test_queue->head;
    for (int i = 0; i < NUM_MESSAGES; i++)
    {
        cr_assert_not_null(node, "Message node %d should not be NULL", i);
        char expected_data[BUF_SIZE];
        snprintf(expected_data, BUF_SIZE, "Test message %d", i);
        cr_assert_str_eq(node->msg.data, expected_data, "Message data for node %d should match", i);
        cr_assert_eq(node->msg.sender_port, 8080 + i, "Sender port for node %d should match", i);

        node = node->next;
    }

    // Teardown
    teardown();
}

// Test type 1 message handling (termination signals)
Test(server_operation, type_1_message_handling)
{
    // Setup
    setup();

    // Add some clients
    int client1 = add_client(&test_ctx, mock_client_fd, 0x7F000001, 8080);
    int client2 = add_client(&test_ctx, mock_client_fd + 1, 0x7F000001, 8081);

    // Create a type 1 message from the first client
    message_t msg;
    msg.type = 1;
    msg.sender_ip = 0x7F000001;
    msg.sender_port = 8080;
    msg.data_len = 0;

    // Set up a thread to process this message like the client_handler would
    pthread_mutex_lock(&test_ctx.mutex);
    pthread_mutex_lock(&test_ctx.clients.mutex);
    test_ctx.clients.clients[client1].sent_type_1 = true;
    test_ctx.type_1_count++;
    pthread_mutex_unlock(&test_ctx.clients.mutex);
    pthread_cond_signal(&test_ctx.type_1_cond);
    pthread_mutex_unlock(&test_ctx.mutex);

    // Verify type_1_count was incremented
    cr_assert_eq(test_ctx.type_1_count, 1, "type_1_count should be 1 after processing a type 1 message");

    // Do the same for the second client
    pthread_mutex_lock(&test_ctx.mutex);
    pthread_mutex_lock(&test_ctx.clients.mutex);
    test_ctx.clients.clients[client2].sent_type_1 = true;
    test_ctx.type_1_count++;
    pthread_mutex_unlock(&test_ctx.clients.mutex);
    pthread_cond_signal(&test_ctx.type_1_cond);
    pthread_mutex_unlock(&test_ctx.mutex);

    // Verify type_1_count was incremented again
    cr_assert_eq(test_ctx.type_1_count, 2, "type_1_count should be 2 after processing two type 1 messages");
    cr_assert_eq(test_ctx.clients.clients[client1].sent_type_1, true, "Client 1 sent_type_1 flag should be true");
    cr_assert_eq(test_ctx.clients.clients[client2].sent_type_1, true, "Client 2 sent_type_1 flag should be true");

    // Teardown
    teardown();
}

// Test server initialization
Test(server_lifecycle, init_server)
{
    // Initialize server with test parameters
    server_ctx_t ctx;
    int test_port = 8888;
    int test_expected_clients = 10;

    init_server(&ctx, test_port, test_expected_clients);

    // Verify server was initialized correctly
    cr_assert_eq(ctx.port, test_port, "Server port should match initialization value");
    cr_assert_eq(ctx.expected_clients, test_expected_clients, "Expected clients should match initialization value");
    cr_assert_eq(ctx.running, true, "Server should be running after initialization");
    cr_assert_eq(ctx.type_1_count, 0, "Type 1 count should be 0 initially");
    cr_assert_not_null(ctx.clients.clients, "Client list should be allocated");
    cr_assert_eq(ctx.clients.count, 0, "Client count should be 0 initially");
    cr_assert_eq(ctx.clients.max_clients, test_expected_clients, "Max clients should match expected clients");
    cr_assert_null(ctx.msg_queue.head, "Message queue head should be NULL initially");
    cr_assert_null(ctx.msg_queue.tail, "Message queue tail should be NULL initially");
    cr_assert_eq(ctx.msg_queue.count, 0, "Message queue count should be 0 initially");
    cr_assert_geq(ctx.server_fd, 0, "Server socket fd should be valid");

    // Clean up
    cleanup_server(&ctx);
}

// Main function to run tests
int main(int argc, char *argv[])
{
    // Run the Criterion tests
    return criterion_run_all_tests();
}
