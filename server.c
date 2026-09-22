#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include <errno.h>
#include "messages.h"

#define LONESHA256_STATIC
#include "lonesha256.h"


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

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Server is listening on port %d...\n", port);
    struct sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);


    while (1) {
        printf("Waiting for a client...\n");

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_address,
            &client_len
        );

        if (client_fd < 0) {
            perror("accept");
            close(server_fd);
            return 1;
        }
        printf("Client connected!\n");


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
            printf("Error total not equal to expected size");
            close(client_fd);
            continue;
        }

        // load the data from the buffer into the appropriate variables
        // hashed value is the first 32 bytes of the buffer
        uint8_t recived_hash[32];
        memcpy(recived_hash, buffer, 32);

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


        // brute force the hash from start to end
        uint64_t i;
        uint64_t anwser;
        for (i = start_value_transformed; i <= end_value_transformed; i++) {
            uint8_t hash[32]; 
            lonesha256(hash, (const unsigned char *)&i, sizeof(i));

            if (memcmp(hash, recived_hash, 32) == 0) {
                anwser = i;
                break;
            }

        }
        

        // send the answer back to the client
        uint64_t anwser_transformed = htobe64(anwser);
        send(client_fd, &anwser_transformed, sizeof(anwser_transformed), 0);
        
        print_hex(recived_hash, sizeof(recived_hash));
        close(client_fd);
        
    }

    close(server_fd);
    return 0;
}