#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdbool.h>
#include "messages.h"
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>

#define LONESHA256_STATIC
#include "lonesha256.h"

#define MAX_WORKERS 10 // The execution server has 16 CPUs, and priority levels max out at 16
#define PRIORITY_LEVELS 16


typedef struct {
    uint64_t answer;
    int found;
} SharedWork;

typedef struct {
    pid_t pid;
    int cpu;
} ManagerStruct;

ManagerStruct managerList[16];
int num_trackers = 0;
int active_cpu = 0;



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


Request *dequeue_highest(bool priorityCheck) {

    // priority_queues[15] = priority 16
    // priority_queues[0]  = priority 1

    for (int i = PRIORITY_LEVELS - 1; i >= 0; i--) {

        if (priority_queues[i].head != NULL) {
            if (priorityCheck) {
                return priority_queues[i].head;
                continue; 
            }

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


int start_request(Request *request, int server_fd) {
    
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");

        close(request->client_fd);
        free(request);

        return -1;
    }

    if (pid > 0) {

        close(request->client_fd);
        free(request);

        return pid;
    }

    printf("Manager process started for priority %d\n", request->priority);
    printf("Manager PID: %d\n", getpid());

    // Map shared memory for the workers
    SharedWork *work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (work == MAP_FAILED) {
        perror("SharedWork failed (mmap)");
        exit(1);
    }

    work->answer = 0;
    work->found = 0;


    pid_t worker_pids[16];
    int num_workers = request->priority;
    if (num_workers < 1) num_workers = 1;
    if (num_workers > 16) num_workers = 16;

    uint64_t total_range = request->end - request->start;
    
    if (num_workers > total_range && total_range > 0) {
        num_workers = total_range;
    }

    uint64_t step_size = total_range / num_workers;


    printf("Priority: %d ", request->priority);
    printf("%d workers\n", num_workers);

    for (int i = 0; i < num_workers; i++) {
        uint64_t start_chunk = request->start + (step_size * i);
        uint64_t end_chunk = (i == num_workers - 1) ? request->end : start_chunk + step_size;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) { 
            close(server_fd); 

            printf("Worker %d processing range [%llu, %llu)\n", i, (unsigned long long)start_chunk, (unsigned long long)end_chunk);

            for (uint64_t j = start_chunk; j < end_chunk; j++) {
                if (work->found) {
                    break;
                }

                uint8_t hash[32]; 
                lonesha256(hash, (const unsigned char *)&j, sizeof(j));
    
                if (memcmp(hash, request->hash, 32) == 0) {
                    work->answer = j;
                    work->found = 1; 
                    printf("Worker %d found answer: %llu\n", i, (unsigned long long)j);
                    break;
                }
            }
            
            munmap(work, sizeof(*work));
            exit(0);
        } else {
            worker_pids[i] = pid;
        }
    }
    


    for (int worker_n = 0; worker_n < num_workers; worker_n++) {
        if (waitpid(worker_pids[worker_n], NULL, 0) < 0 ) {
            perror("wait error in loop");
        }
    }

    // Send the result
    uint64_t answer_transformed = htobe64(work->answer);
    send(request->client_fd, &answer_transformed, sizeof(answer_transformed), 0);

    close(request->client_fd);
    // Clean up shared memory in parent
    munmap(work, sizeof(*work)); 

    exit(0);
}


int main(int argc, char *argv[]) {

    int port;
    if (argc < 2) {
        port = 5003;
    } else {
        port = atoi(argv[1]);
    }
    printf("Starting server on port %d...\n", port);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
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


    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 1000) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Server is listening on port %d...\n", port);
    struct sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);

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



    while (1) {

        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, NULL, WNOHANG)) > 0) {
            for (int i = 0; i < num_trackers; i++) {
                if (managerList[i].pid == dead_pid) {
                    active_cpu -= managerList[i].cpu;
                    managerList[i] = managerList[num_trackers - 1];
                    num_trackers--;
                    break;
                }
            }
        }



        while (1) { 

            Request *next_request = dequeue_highest(true);
            if (next_request == NULL) {
                break; 
            }

            int required_cpu = next_request->priority;
            if (required_cpu != 16) {
                required_cpu += 1;
            }

            if (active_cpu + required_cpu > 16) {
                break;
            }

            next_request = dequeue_highest(false);
            
            pid_t manager_pid = start_request(next_request, server_fd);

            if (manager_pid > 0) {
                managerList[num_trackers].pid = manager_pid;
                managerList[num_trackers].cpu = required_cpu;
                num_trackers++;
                active_cpu += required_cpu;
                }

        }

        int client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            perror("accept");
            continue;
        }

        // MAC shit outcomment when push to main!!!!
        int c_flags = fcntl(client_fd, F_GETFL, 0);
        if (c_flags >= 0) {
            fcntl(client_fd, F_SETFL, c_flags & ~O_NONBLOCK);
        }
        // MAC shit outcomment when push to main!!!!

        printf("Client connected!\n");

        // Receive the data from the client
        // The expected size of the data is 49 bytes (32 bytes hash + 8 bytes start + 8 bytes end + 1 byte p)
        uint8_t buffer[49];
        size_t total_received = 0;
        while (total_received < sizeof(buffer)) {
            ssize_t bytes_received = recv(client_fd, buffer + total_received, sizeof(buffer) - total_received, 0);
            if (bytes_received <= 0) {
                perror("Socket closed or error during recv");
                break;
            }
            total_received += bytes_received;
        }

        // fail if not fully received
        if (total_received != sizeof(buffer)) {
            printf("Error: total received not equal to expected size\n");
            close(client_fd);
            continue;
        }

        // load the data from the buffer into the appropriate variables
        // hashed value is the first 32 bytes of the buffer
        uint8_t received_hash[32];
        memcpy(received_hash, buffer, 32);

        // start value is the next 8 bytes of the buffer
        uint64_t start_value;
        memcpy(&start_value, buffer + 32, 8);
        // transformed due to endianness
        uint64_t start_value_transformed = be64toh(start_value);

        // end value is the next 8 bytes of the buffer
        uint64_t end_value;
        memcpy(&end_value, buffer + 40, 8);
        // transformed due to endianness
        uint64_t end_value_transformed = be64toh(end_value);

        // p value is the last byte of the buffer
        uint8_t p_value;
        memcpy(&p_value, buffer + 48, 1);


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

        enqueue(request);


        printf(
            "[QUEUE] priority=%u range=[%llu,%llu)\n",
            request->priority,
            (unsigned long long)request->start,
            (unsigned long long)request->end
        );


    }

    close(server_fd);
    return 0;
}