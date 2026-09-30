#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include <errno.h>
#include "messages.h"
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <sys/mman.h>

#define LONESHA256_STATIC
#include "lonesha256.h"

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t answer;
    int found;
} SharedWork;

/*
4 forks max becuase the CPUs on our VMs seems to be 4 threads,
needs to be set at 16 when submiting due to project specifications
*/
#define CPU_THREADS 10
#define WORKER 5
#define MAX_CHILDREN (CPU_THREADS / WORKER)


// for testing. can be removed later
void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
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

    printf("Socket created successfully!\n");
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

    printf("Socket bound to port %d successfully!\n", port);

    if (listen(server_fd, 1000) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Server is listening on port %d...\n", port);
    struct sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);

    int active_children = 0;
    pid_t worker_pids[WORKER];

    while (1) {
        
        printf("Waiting for a client...\n");

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_address,
            &client_len
        );

        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        printf("Client connected!\n");

        SharedWork *work = mmap(NULL, sizeof(*work), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        
        if (work == MAP_FAILED) {
            perror("SharedWork failed (mmap)");
            close(server_fd);
            exit(1);
        }

        // Receive the data from the client
        // The expected size of the data is 49 bytes (32 bytes hash + 8 bytes start + 8 bytes end + 1 byte p)
        uint8_t buffer[49];
        size_t total_received = 0;
        while (total_received < sizeof(buffer)) {
            ssize_t bytes_received = recv(client_fd, buffer + total_received, sizeof(buffer) - total_received, 0);
            
            if ((bytes_received < 0) || (bytes_received == 0)) {
                perror("nothing received yet will fail");
                break;
            }
            total_received += bytes_received;
        }
        
        // fail if not fully received
        if (total_received != sizeof(buffer)) {
            printf("Error total not equal to expected size\n");
            close(client_fd);
            exit(0);
        }

        // load the data from the buffer into the appropriate variables
        // hashed value is the first 32 bytes of the buffer
        uint8_t received_hash[32];
        memcpy(received_hash, buffer, 32);

        // start value is the next 8 bytes of the buffer
        uint64_t start_value;
        memcpy(&start_value, buffer + 32, 8);
        // transformed due to endianness
        uint64_t start_value_transformed;
        start_value_transformed = be64toh(start_value);

        // end value is the next 8 bytes of the buffer
        uint64_t end_value;
        memcpy(&end_value, buffer + 40, 8);
        // transformed due to endianness
        uint64_t end_value_transformed;
        end_value_transformed = be64toh(end_value);

        // p value is the last byte of the buffer
        uint8_t p_value;
        memcpy(&p_value, buffer + 48, 1);

        work->start = start_value_transformed;
        work->end = end_value_transformed;
        work->answer = 0;
        work->found = 0;

        uint64_t step_size = (end_value_transformed - start_value_transformed) / WORKER;

        int i;
        for (i=0; i<WORKER; i++) {

            uint64_t start_chunk = start_value_transformed + (step_size * i);
            uint64_t end_chunk = start_value_transformed + (step_size * (i+1));
    
            pid_t pid = fork();
            printf("\npid=%d\n", (int)pid);

            if (pid < 0) {
                perror("fork");
                close(client_fd);
                exit(1);
            }

            if (pid == 0) {
                if (work->found) {
                    exit(0);
                }
                printf("Worker %d, PID %d\n", i, (int)getpid());
                // close server once a fork has been done
                close(server_fd);

                // brute force the hash from start to end
                uint64_t i;
                for (i = start_chunk; i < end_chunk; i++) {
                    if (work->found) {
                        break;
                    }

                    uint8_t hash[32]; 
                    lonesha256(hash, (const unsigned char *)&i, sizeof(i));
        
                    if (memcmp(hash, received_hash, 32) == 0) {
                        work->answer = i;
                        work->found = 1;
                        printf("answer found : %llu \n", i);
                        exit(0);
                    }
                }
                close(client_fd);
                exit(0);

            } else {
                worker_pids[i] = pid;
                active_children++;
                
            }
            
        }
        
        for (int worker_n = 0; worker_n < WORKER; worker_n++) {

            if (waitpid(worker_pids[worker_n], NULL, 0) < 0 ) {
                perror("wait error 2 loop");
            }

            
        }
        uint64_t answer_transformed = htobe64(work->answer);
        print_hex(received_hash, sizeof(received_hash));
        send(client_fd, &answer_transformed, sizeof(answer_transformed), 0);

        close(client_fd);
        munmap(work, sizeof(*work));
 
    }

    close(server_fd);
    return 0;
}
