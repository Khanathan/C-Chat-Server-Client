# C Multi-Client Chat Server
A robust, multi-threaded socket-based messaging system that enables multiple clients to connect to a centralized chat server.

## Overview
This project implements a complete client-server architecture with the following features:
- Multi-threaded design: Both client and server use separate threads for handling message sending, receiving, and processing
- Non-blocking I/O: Socket operations are non-blocking for better performance
- Message queue mechanism: Uses a thread-safe message queue for handling incoming messages
- Connection management: Handles multiple client connections simultaneously using TCP sockets.

## Components
### Server
The server component receives connections from multiple clients, processes their messages, and broadcasts them to all connected clients.
Key features:
- Accepts and manages multiple client connections
- Thread-safe client list management
- Processes and broadcasts messages
- Coordinates shutdown when all clients have finished sending messages
  
### Client
The client component is a fuzzer (mainly for testing the server) which connects to the server, sends a specified number of random messages, and receives messages from all clients.

Key features:
- Simultaneous message sending and receiving using threads
- Logs received messages to a specified file
- Generates random content for test messages
- Terminates gracefully after sending specified number of messages

I will definitely have to come back to this project at some point and complete a normal client to use with the server.

## Message Protocol
Messages have a simple format:
1. Type byte: 0 for regular messages, 1 for termination signals
2. Sender information: For type 0 messages, contains sender IP and port
3. Message content: The actual message data
4. Terminator: Newline character (\n)

## Usage
### Server
```./server <port number> <# of clients>```

Parameters:
- port number: The port on which the server will listen for connections
- \# of clients: The number of expected client connections
  
### Client
```./client <IP address> <port number> <# of messages> <log file path>```

Parameters:
- IP address: IP address of the server
- port number: Port number the server is listening on
- \# of messages: Number of messages to send before terminating
- log file path: Path to file where received messages will be logged
  
## Build Instructions
Compile the project using the following commands:
```
# Compile server
gcc -o server server.c -pthread

# Compile client
gcc -o client client.c -pthread
```

## Example Session

## Technical Implementation
