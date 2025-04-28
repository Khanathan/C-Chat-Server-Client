#include <criterion/criterion.h>
#include <criterion/new/assert.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "../include/server.h"

// Declare a test suite with setup and teardown
TestSuite(server, .init = setup, .fini = teardown);

// Global variables for testing (avoid if possible; moved to setup where feasible)
static server_ctx_t test_ctx;
static int mock_client_fd = 100;
static msg_queue_t *test_queue = NULL;

void setup(void)
{
    init_server(&test_ctx, 8080, 5);
    test_queue = &test_ctx.msg_queue;
}

void teardown(void)
{
    cleanup_server(&test_ctx);
}

Test(server, make_socket_nonblocking)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert(sock >= 0, "Failed to create test socket");

    int result = make_socket_nonblocking(sock);
    cr_assert_eq(result, 0, "make_socket_nonblocking should return 0");

    int flags = fcntl(sock, F_GETFL, 0);
    cr_assert(flags & O_NONBLOCK, "Socket should have O_NONBLOCK flag");

    close(sock);
}

Test(server, add_client)
{
    int idx = add_client(&test_ctx, mock_client_fd, 0x7F000001, 8080);
    cr_assert_geq(idx, 0, "add_client should return a non-negative index");
    cr_assert_eq(test_ctx.clients.count, 1, "Client count should be 1");
    cr_assert_eq(test_ctx.clients.clients[idx].fd, mock_client_fd, "Client fd should match");
    cr_assert_eq(test_ctx.clients.clients[idx].ip, 0x7F000001, "Client IP should match");
    cr_assert_eq(test_ctx.clients.clients[idx].port, 8080, "Client port should match");
    cr_assert_eq(test_ctx.clients.clients[idx].sent_type_1, false, "sent_type_1 should be false");
}

Test(server, add_client_max_exceeded)
{
    for (int i = 0; i < test_ctx.clients.max_clients; i++)
    {
        int idx = add_client(&test_ctx, mock_client_fd + i, 0x7F000001, 8080 + i);
        cr_assert_geq(idx, 0, "add_client should succeed for client %d", i);
    }

    int idx = add_client(&test_ctx, mock_client_fd + 100, 0x7F000001, 9000);
    cr_assert_eq(idx, -1, "add_client should return -1 when max clients exceeded");
    cr_assert_eq(test_ctx.clients.count, test_ctx.clients.max_clients, "Client count should remain at max");
}

Test(server, enqueue_message)
{
    message_t msg = {
        .type = 0,
        .sender_ip = 0x7F000001,
        .sender_port = 8080,
        .data_len = snprintf(msg.data, BUF_SIZE, "Test message")};

    enqueue_message(test_queue, &msg);

    cr_assert_eq(test_queue->count, 1, "Queue count should be 1");
    cr_assert_not_null(test_queue->head, "Queue head should not be NULL");
    cr_assert_not_null(test_queue->tail, "Queue tail should not be NULL");

    msg_node_t *node = test_queue->head;
    cr_assert_eq(node->msg.type, msg.type, "Message type should match");
    cr_assert_eq(node->msg.sender_ip, msg.sender_ip, "Sender IP should match");
    cr_assert_eq(node->msg.sender_port, msg.sender_port, "Sender port should match");
    cr_assert_str_eq(node->msg.data, msg.data, "Message data should match");
}

Test(server, enqueue_multiple_messages)
{
    const int NUM_MESSAGES = 5;
    for (int i = 0; i < NUM_MESSAGES; i++)
    {
        message_t msg = {
            .type = 0,
            .sender_ip = 0x7F000001,
            .sender_port = 8080 + i,
            .data_len = snprintf(msg.data, BUF_SIZE, "Test message %d", i)};
        enqueue_message(test_queue, &msg);
    }

    cr_assert_eq(test_queue->count, NUM_MESSAGES, "Queue count should be %d", NUM_MESSAGES);

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
}

Test(server, type_1_message_handling)
{
    int client1 = add_client(&test_ctx, mock_client_fd, 0x7F000001, 8080);
    int client2 = add_client(&test_ctx, mock_client_fd + 1, 0x7F000001, 8081);

    pthread_mutex_lock(&test_ctx.mutex);
    pthread_mutex_lock(&test_ctx.clients.mutex);
    test_ctx.clients.clients[client1].sent_type_1 = true;
    test_ctx.type_1_count++;
    pthread_mutex_unlock(&test_ctx.clients.mutex);
    pthread_cond_signal(&test_ctx.type_1_cond);
    pthread_mutex_unlock(&test_ctx.mutex);

    cr_assert_eq(test_ctx.type_1_count, 1, "type_1_count should be 1");

    pthread_mutex_lock(&test_ctx.mutex);
    pthread_mutex_lock(&test_ctx.clients.mutex);
    test_ctx.clients.clients[client2].sent_type_1 = true;
    test_ctx.type_1_count++;
    pthread_mutex_unlock(&test_ctx.clients.mutex);
    pthread_cond_signal(&test_ctx.type_1_cond);
    pthread_mutex_unlock(&test_ctx.mutex);

    cr_assert_eq(test_ctx.type_1_count, 2, "type_1_count should be 2");
    cr_assert_eq(test_ctx.clients.clients[client1].sent_type_1, true, "Client 1 sent_type_1 should be true");
    cr_assert_eq(test_ctx.clients.clients[client2].sent_type_1, true, "Client 2 sent_type_1 should be true");
}

Test(server, init_server)
{
    server_ctx_t ctx;
    int test_port = 8888;
    int test_expected_clients = 10;

    init_server(&ctx, test_port, test_expected_clients);

    cr_assert_eq(ctx.port, test_port, "Server port should match");
    cr_assert_eq(ctx.expected_clients, test_expected_clients, "Expected clients should match");
    cr_assert_eq(ctx.running, true, "Server should be running");
    cr_assert_eq(ctx.type_1_count, 0, "Type 1 count should be 0");
    cr_assert_not_null(ctx.clients.clients, "Client list should be allocated");
    cr_assert_eq(ctx.clients.count, 0, "Client count should be 0");
    cr_assert_eq(ctx.clients.max_clients, test_expected_clients, "Max clients should match");
    cr_assert_null(ctx.msg_queue.head, "Message queue head should be NULL");
    cr_assert_null(ctx.msg_queue.tail, "Message queue tail should be NULL");
    cr_assert_eq(ctx.msg_queue.count, 0, "Message queue count should be 0");
    cr_assert_geq(ctx.server_fd, 0, "Server socket fd should be valid");

    cleanup_server(&ctx);
}