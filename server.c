#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "messages.h"

#define LONESHA256_STATIC
#include "lonesha256.h"

#define MAX_CHILDREN 2
#define PRIORITY_LEVELS 16
#define REQUEST_SIZE 49


// ------------------------------------------------------------
// Request / Priority Queue structures
// ------------------------------------------------------------

typedef struct Request {
    int client_fd;

    uint8_t hash[32];
    uint64_t start;
    uint64_t end;
    uint8_t priority;

    struct Request *next;
} Request;


typedef struct {
    Request *head;
    Request *tail;
} Queue;


Queue priority_queues[PRIORITY_LEVELS] = {0};


// ------------------------------------------------------------
// Priority Queue
// ------------------------------------------------------------

void enqueue(Request *request) {
    int index = request->priority - 1;

    request->next = NULL;

    if (priority_queues[index].tail == NULL) {

        // Queue is empty
        priority_queues[index].head = request;
        priority_queues[index].tail = request;

    } else {

        // Add to end of queue
        priority_queues[index].tail->next = request;
        priority_queues[index].tail = request;
    }
}


Request *dequeue_highest(void) {

    // priority_queues[15] = priority 16
    // priority_queues[0]  = priority 1

    for (int i = PRIORITY_LEVELS - 1; i >= 0; i--) {

        if (priority_queues[i].head != NULL) {

            Request *request = priority_queues[i].head;

            priority_queues[i].head = request->next;

            if (priority_queues[i].head == NULL) {
                priority_queues[i].tail = NULL;
            }

            request->next = NULL;

            return request;
        }
    }

    return NULL;
}


// ------------------------------------------------------------
// Process one request
// ------------------------------------------------------------

int start_request(Request *request, int server_fd) {

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");

        close(request->client_fd);
        free(request);

        return -1;
    }


    // --------------------------------------------------------
    // PARENT
    // --------------------------------------------------------

    if (pid > 0) {

        /*
         * The child has its own copy of the file descriptor and
         * Request structure after fork().
         */

        close(request->client_fd);
        free(request);

        return 1;
    }


    // --------------------------------------------------------
    // CHILD
    // --------------------------------------------------------

    close(server_fd);

    uint64_t answer = 0;


    // Search [start, end)
    for (uint64_t i = request->start;
         i < request->end;
         i++) {

        uint8_t hash[32];

        /*
         * The assignment specifies that the uint64_t fed into
         * SHA256 is little-endian.
         */
        uint64_t little_i = htole64(i);

        lonesha256(
            hash,
            (const unsigned char *)&little_i,
            sizeof(little_i)
        );


        if (memcmp(
                hash,
                request->hash,
                sizeof(request->hash)
            ) == 0) {

            answer = i;
            break;
        }
    }


    // Response must be big-endian
    uint64_t answer_network = htobe64(answer);


    ssize_t sent = send(
        request->client_fd,
        &answer_network,
        sizeof(answer_network),
        0
    );

    if (sent < 0) {
        perror("send");
    }


    close(request->client_fd);
    free(request);

    exit(0);
}


// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main(int argc, char *argv[]) {

    int port;

    if (argc < 2) {
        port = 5003;
    } else {
        port = atoi(argv[1]);
    }


    printf("Starting server on port %d...\n", port);


    // --------------------------------------------------------
    // Create socket
    // --------------------------------------------------------

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }


    /*
     * Makes restarting the server easier if the previous socket
     * is still in TIME_WAIT.
     */
    int opt = 1;

    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)
        ) < 0) {

        perror("setsockopt");
        close(server_fd);
        return 1;
    }


    // --------------------------------------------------------
    // Server address
    // --------------------------------------------------------

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);


    // --------------------------------------------------------
    // Bind
    // --------------------------------------------------------

    if (bind(
            server_fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0) {

        perror("bind");
        close(server_fd);
        return 1;
    }


    printf("Socket bound to port %d successfully!\n", port);


    // --------------------------------------------------------
    // Listen
    // --------------------------------------------------------

    if (listen(server_fd, 1000) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }


    printf("Server is listening on port %d...\n", port);


    // --------------------------------------------------------
    // Make listening socket non-blocking
    // --------------------------------------------------------

    int flags = fcntl(server_fd, F_GETFL, 0);

    if (flags < 0) {
        perror("fcntl F_GETFL");
        close(server_fd);
        return 1;
    }


    if (fcntl(
            server_fd,
            F_SETFL,
            flags | O_NONBLOCK
        ) < 0) {

        perror("fcntl F_SETFL");
        close(server_fd);
        return 1;
    }


    // --------------------------------------------------------
    // Client information
    // --------------------------------------------------------

    struct sockaddr_in client_address;
    socklen_t client_len;

    int active_children = 0;


    // ========================================================
    // MAIN SERVER LOOP
    // ========================================================

    while (1) {

        // ----------------------------------------------------
        // 1. Check if any children have finished
        // ----------------------------------------------------

        while (waitpid(-1, NULL, WNOHANG) > 0) {

            if (active_children > 0) {
                active_children--;
            }
        }


        // ----------------------------------------------------
        // 2. Fill available CPU/child slots from queue
        // ----------------------------------------------------

        while (active_children < MAX_CHILDREN) {

            Request *next = dequeue_highest();

            if (next == NULL) {
                break;
            }


            printf(
                "[SCHEDULE] priority=%u\n",
                next->priority
            );


            int result = start_request(next, server_fd);

            if (result > 0) {
                active_children++;
            }
        }


        // ----------------------------------------------------
        // 3. Try accepting another client
        // ----------------------------------------------------

        client_len = sizeof(client_address);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_address,
            &client_len
        );


        if (client_fd < 0) {

            /*
             * Since the socket is non-blocking, these mean:
             *
             * "There simply isn't another connection right now."
             *
             * That is not an error.
             */

            if (errno != EAGAIN &&
                errno != EWOULDBLOCK) {

                perror("accept");
            }


            /*
             * Prevent this loop from burning an entire CPU core
             * while no connections are available.
             */
            usleep(1000);

            continue;
        }


        printf("Client connected!\n");


        // ----------------------------------------------------
        // 4. Receive exactly 49 bytes
        // ----------------------------------------------------

        uint8_t buffer[REQUEST_SIZE];

        size_t total_received = 0;


        while (total_received < sizeof(buffer)) {

            ssize_t bytes_received = recv(
                client_fd,
                buffer + total_received,
                sizeof(buffer) - total_received,
                0
            );


            if (bytes_received < 0) {

                if (errno == EINTR) {
                    continue;
                }

                perror("recv");
                break;
            }


            if (bytes_received == 0) {
                break;
            }


            total_received += bytes_received;
        }


        if (total_received != sizeof(buffer)) {

            printf(
                "Invalid request size: received %zu bytes\n",
                total_received
            );

            close(client_fd);

            continue;
        }


        // ----------------------------------------------------
        // 5. Build Request
        // ----------------------------------------------------

        Request *request = malloc(sizeof(Request));

        if (request == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }


        request->client_fd = client_fd;
        request->next = NULL;


        // SHA256 hash: bytes 0-31
        memcpy(
            request->hash,
            buffer,
            sizeof(request->hash)
        );


        // start: bytes 32-39
        uint64_t start_network;

        memcpy(
            &start_network,
            buffer + 32,
            sizeof(start_network)
        );

        request->start = be64toh(start_network);


        // end: bytes 40-47
        uint64_t end_network;

        memcpy(
            &end_network,
            buffer + 40,
            sizeof(end_network)
        );

        request->end = be64toh(end_network);


        // priority: byte 48
        request->priority = buffer[48];


        // ----------------------------------------------------
        // 6. Validate priority
        // ----------------------------------------------------

        if (request->priority < 1 ||
            request->priority > 16) {

            printf(
                "Invalid priority: %u\n",
                request->priority
            );

            close(client_fd);
            free(request);

            continue;
        }


        // ----------------------------------------------------
        // 7. Add request to priority queue
        // ----------------------------------------------------

        enqueue(request);


        printf(
            "[QUEUE] priority=%u range=[%llu,%llu)\n",
            request->priority,
            (unsigned long long)request->start,
            (unsigned long long)request->end
        );


        /*
         * DO NOT fork here.
         *
         * Go back to the top of the loop.
         *
         * The scheduler will choose the highest-priority queued
         * request whenever a child slot is available.
         */
    }


    close(server_fd);

    return 0;
}