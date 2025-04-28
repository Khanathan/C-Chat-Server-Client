#include "server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_BACKLOG 32
#define BUF_SIZE 1024

struct msgIncoming
{
  uint8_t type;
  char *msg_str;
};

struct msgOutgoing0
{
  uint8_t type;
  uint32_t addr;
  uint16_t port;
  char *msg_str;
};

struct msgOutgoing1
{
  uint8_t type;
  char *msg_str;
};

struct list_node
{
  struct list_node *next;
  void *data;
  uint32_t addr;
  uint16_t port;
};

struct list_handle
{
  struct list_node *last;
  volatile uint32_t count;
};

struct acceptor_args
{
  atomic_bool run;
  int port;
  int numOfClients;
  int *numClients;
  int *cfds;
  struct list_handle *list_handle;
  pthread_mutex_t *list_lock;
};

struct processor_args
{
  atomic_bool run;
  int *type_1_count;
  struct list_node *head;
  pthread_mutex_t *list_lock;
  pthread_cond_t *list_cond;
  pthread_cond_t *type_1_cond;
  int *numClients; // pointer to the number of clients currently connected
  int *cfds;       // pointer to array of cfd
};

struct client_args
{
  atomic_bool run;
  int cfd;

  struct list_handle *list_handle;
  pthread_mutex_t *list_lock;

  uint32_t addr;
  uint16_t port;
};

void handle_error(char *msg)
{
  do
  {
    perror(msg);
    exit(EXIT_FAILURE);
  } while (0);
}

int init_server_socket(int port)
{
  struct sockaddr_in addr;
  int sfd;

  sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1)
  {
    handle_error("socket");
  }

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1)
  {
    handle_error("bind");
  }

  if (listen(sfd, LISTEN_BACKLOG) == -1)
  {
    handle_error("listen");
  }
  return sfd;
}

void set_non_blocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
  {
    handle_error("fcntl F_GETFL");
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
  {
    handle_error("fcntl F_SETFL");
  }
}

void add_to_list(struct list_handle *list_handle, struct list_node *new_node)
{
  struct list_node *last_node = list_handle->last;
  last_node->next = new_node;
  list_handle->last = last_node->next;
  list_handle->count++;
}

// Handles receiving msg from client and appending it to the list of msgs
static void *run_client(void *args)
{
  struct client_args *cargs = (struct client_args *)args;
  int cfd = cargs->cfd;
  set_non_blocking(cfd);

  char msg_buf[BUF_SIZE];

  // the main loop
  while (cargs->run)
  {
    ssize_t bytes_read = read(cfd, &msg_buf, BUF_SIZE);
    if (bytes_read == -1)
    {
      if (!(errno == EAGAIN || errno == EWOULDBLOCK))
      {
        handle_error("Problem reading from socket");
        break;
      }
    }
    else if (bytes_read > 0)
    {
      printf("creating node.\n");
      // create node with data from socket
      struct list_node *new_node = malloc(sizeof(struct list_node));
      new_node->next = NULL;
      new_node->data = malloc(BUF_SIZE);
      new_node->addr = cargs->addr;
      new_node->port = cargs->port;
      memcpy(new_node->data, msg_buf, BUF_SIZE);

      // Append node to list, maybe signal the msg handler
      pthread_mutex_lock(cargs->list_lock);
      {
        add_to_list(cargs->list_handle, new_node);
      }
      pthread_mutex_unlock(cargs->list_lock);
    }
  }

  // Exits loop when run set to false
  if (close(cfd) == -1)
  {
    handle_error("close cfd");
  }

  // All done
  return NULL;
}

// Handles accepting client connections and creating client handler threads
// Has access to the list of client_threads originally in main
static void *run_acceptor(void *args)
{
  struct acceptor_args *aargs = (struct acceptor_args *)args;

  int sfd = init_server_socket(aargs->port);
  set_non_blocking(sfd);

  pthread_mutex_t *list_lock = aargs->list_lock;
  struct list_handle *list_handle = aargs->list_handle;

  pthread_t client_threads[aargs->numOfClients];
  struct client_args client_args[aargs->numOfClients];
  int *numClients = aargs->numClients;
  int *cfds = aargs->numClients;

  printf("numClients = %d\n", *numClients);
  // loop for accepting
  while (aargs->run)
  {
    if (*numClients < aargs->numOfClients)
    {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);

      int cfd = accept(sfd, (struct sockaddr *)&client_addr, &client_len);
      if (cfd == -1)
      {
        if (!(errno == EAGAIN || errno == EWOULDBLOCK))
        {
          handle_error("accept");
        }
      }
      else
      {
        cfds[*numClients] = cfd;
        printf("%dth Client connected!\n", *numClients);
        client_args[*numClients].run = true;
        client_args[*numClients].cfd = cfd;
        client_args[*numClients].list_handle = list_handle;
        client_args[*numClients].list_lock = list_lock;
        client_args[*numClients].addr = ntohl(client_addr.sin_addr.s_addr);
        client_args[*numClients].port = ntohs(client_addr.sin_port);

        // create thread for handling client
        pthread_create(&client_threads[*numClients], NULL, run_client,
                       &client_args[*numClients]);
        *numClients += 1;
      }
    }
  }

  // Once enough type 1 msgs received, the while loop is exited
  // run flag is set to false by main
  // Commence cleanup
  for (int i = 0; i < *numClients; i++)
  {
    client_args[i].run = false;
    pthread_join(client_threads[i], NULL);
  }

  // All work done, terminate
  if (close(sfd) == -1)
  {
    handle_error("close sfd");
  }

  return NULL;
}

int myLen(char *str)
{
  int len = 0;
  int max = 1024;
  while (len < max && str[len] != '\n')
  {
    len++;
  }
  len++;
  return len;
}

static void *run_processor(void *args)
{
  struct processor_args *pargs = (struct processor_args *)args;
  pthread_cond_t *list_cond = pargs->list_cond;
  pthread_mutex_t *list_lock = pargs->list_lock;
  int *cfds = pargs->cfds;
  int *numClients = pargs->numClients;

  pthread_mutex_lock(list_lock);
  {
    // cond wait while list is empty
    while (pargs->head == NULL)
    {
      pthread_cond_wait(list_cond, list_lock);
    }

    // let it rip
    // processList;
    struct list_node *currNode = pargs->head;

    while (currNode != NULL)
    {
      struct msgIncoming *msgIncoming = (struct msgIncoming *)currNode->data;
      // type check
      if (msgIncoming->type == 0)
      {
        // put everything into a buffer and send in one go
        char *buffer = malloc(sizeof(char) * BUF_SIZE);
        size_t offset = 0;
        buffer[offset] = 0;
        offset += sizeof(msgIncoming->type);

        uint32_t addr_net = htonl(currNode->addr);
        memcpy(buffer + offset, &addr_net, sizeof(addr_net));
        offset += sizeof(addr_net);

        uint16_t port_net = htons(currNode->port);
        memcpy(buffer + offset, &port_net, sizeof(port_net));
        offset += sizeof(port_net);

        // deal with the msg string
        //  find the len up to \n
        int len = myLen(msgIncoming->msg_str);
        memcpy(buffer + offset, msgIncoming->msg_str, len);

        size_t total_size = sizeof(msgIncoming->type) + sizeof(addr_net) +
                            sizeof(port_net) + len;

        for (int i = 0; i < *numClients; i++)
        {
          ssize_t bytes_sent = send(cfds[i], buffer, total_size, 0);
          if (bytes_sent != (ssize_t)total_size)
          {
            handle_error("Send message to client.");
          }
        }
        free(buffer);
      }
      else if (msgIncoming->type == 1)
      {
        // increment counter and signal cond for main to check
        *(pargs->type_1_count) += 1;
        pthread_cond_signal(pargs->type_1_cond);
      }
      struct list_node *temp = currNode;
      currNode = currNode->next;
      free(temp);
    }
  }
  pthread_mutex_unlock(list_lock);

  return NULL;
}

int main(int argc, char *argv[])
{
  pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t list_cond = PTHREAD_COND_INITIALIZER;
  pthread_cond_t type_1_cond = PTHREAD_COND_INITIALIZER;

  // List head shan't be freed
  struct list_node *head = NULL;
  struct list_handle list_handle = {
      .last = head,
      .count = 0,
  };

  char portArg[32];
  strcpy(portArg, argv[1]);
  char nocArg[32];
  strcpy(nocArg, argv[2]);

  int port = atoi(portArg);
  int numOfClients = atoi(nocArg);
  int numClients = 0;
  int type_1_count = 0;
  int cfds[numOfClients];

  struct acceptor_args aargs = {
      .run = true,
      .port = port,
      .numOfClients = numOfClients,
      .numClients = &numClients,
      .list_handle = &list_handle,
      .list_lock = &list_lock,
      .cfds = cfds,
  };

  // Create a thread that accepts client connections
  //   This thread will create more thread that receives client messages for
  //   each connected client
  pthread_t acceptor_thread;
  pthread_create(&acceptor_thread, NULL, run_acceptor, &aargs);

  // Create a thread for processing messages
  struct processor_args pargs = {
      .run = true,
      .head = head,
      .type_1_count = &type_1_count,
      .list_lock = &list_lock,
      .list_cond = &list_cond,
      .type_1_cond = &type_1_cond,
      .numClients = &numClients,
      .cfds = cfds,
  };

  pthread_t processor_thread;
  pthread_create(&processor_thread, NULL, run_processor, &pargs);

  // Keep track of how many type 1 messages the server has received
  // Once it reaches the same number as aargs->numClients (num of clients
  // currently connected), start the shutdown process
  pthread_mutex_lock(&list_lock);

  while (type_1_count == 0 || type_1_count < numClients)
  {
    printf("in loop.\n");
    pthread_cond_wait(&type_1_cond, &list_lock);
  }

  pthread_mutex_unlock(&list_lock);

  // loop exits when all clients have sent a type 1 msg
  // Start shutting things down

  aargs.run = false;
  pthread_join(acceptor_thread, NULL);
  pthread_join(processor_thread, NULL);
  pthread_mutex_destroy(&list_lock);
  pthread_cond_destroy(&list_cond);
  return 0;
}
