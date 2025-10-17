#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <pthread.h>
#endif

#define RESPONSE_SIZE 8192

static volatile int running = 1;

// Thread that monitors server socket using select() and prints messages as they arrive
void *receiver_thread(void *arg) {
    int sock = *(int *)arg;

    while (running) {
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        tv.tv_sec = 1; // timeout so we can check the running flag periodically
        tv.tv_usec = 0;

        int rv = select(sock + 1, &read_fds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("select");
            break;
        } else if (rv == 0) {
            continue; // timeout - loop again
        }

        if (FD_ISSET(sock, &read_fds)) {
            char buffer[RESPONSE_SIZE];
            int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) {
                printf("Server closed connection or error occurred (recv <= 0).\n");
                running = 0;
                break;
            }
            buffer[bytes] = '\0';
            printf("\n%s\n", buffer);
            fflush(stdout);
        }
    }

    return NULL;
}

// Helper: send a command and wait for a single response (used for synchronous login)
int send_command_and_wait_response(int sock, const char *cmd, char *out, size_t outlen) {
    if (send(sock, cmd, strlen(cmd), 0) < 0) {
        perror("send");
        return -1;
    }

    int total = 0;
    while (total < (int)outlen - 1) {
        int n = recv(sock, out + total, outlen - total - 1, 0);
        if (n <= 0) break;
        total += n;
        // naive stop: if recv returned less than buffer remaining, assume end
        if (n < (int)outlen - total - 1) break;
    }
    out[total] = '\0';
    return total;
}

int main() {

#ifdef _WIN32
    // Initialize Winsock for Windows network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    int netSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (netSocket < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9001);
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

    int connectStatus = connect(netSocket, (struct sockaddr *)&server_address, sizeof(server_address));
    if (connectStatus < 0) {
        perror("Connection failed");
#ifdef _WIN32
        closesocket(netSocket);
        WSACleanup();
#else
        close(netSocket);
#endif
        return 1;
    }

    // Receive initial prompt
    char serverResponse[RESPONSE_SIZE] = {0};
    int r = recv(netSocket, serverResponse, sizeof(serverResponse) - 1, 0);
    if (r > 0) {
        serverResponse[r] = '\0';
        printf("%s\n", serverResponse);
    }

    // Synchronous login loop: require the user to LOGIN before starting concurrent IO
    char userInput[512];
    char response[RESPONSE_SIZE];
    while (running) {
        printf("Enter LOGIN <username> <password>: ");
        if (!fgets(userInput, sizeof(userInput), stdin)) {
            running = 0;
            break;
        }
        userInput[strcspn(userInput, "\n")] = 0;
        if (strncmp(userInput, "LOGIN", 5) != 0) {
            printf("Please send a LOGIN command first.\n");
            continue;
        }

        // send and wait for response
        memset(response, 0, sizeof(response));
        int len = send_command_and_wait_response(netSocket, userInput, response, sizeof(response));
        if (len <= 0) {
            printf("Failed to receive response from server during login.\n");
            running = 0;
            break;
        }

        printf("%s\n", response);
        if (strstr(response, "200 OK") != NULL) {
            printf("Login successful.\n");
            break;
        }
    }

    if (!running) {
#ifdef _WIN32
        closesocket(netSocket);
        WSACleanup();
#else
        close(netSocket);
#endif
        return 0;
    }

    // Start receiver thread to monitor server messages concurrently
    pthread_t tid;
    if (pthread_create(&tid, NULL, receiver_thread, &netSocket) != 0) {
        perror("pthread_create");
        running = 0;
    }

    // Main thread: use select() to monitor stdin and send user input to server
    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int rv = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("select");
            break;
        } else if (rv == 0) {
            continue; // timeout
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (!fgets(userInput, sizeof(userInput), stdin)) {
                running = 0;
                break;
            }
            userInput[strcspn(userInput, "\n")] = 0;

            if (strlen(userInput) == 0) continue;

            if (send(netSocket, userInput, strlen(userInput), 0) < 0) {
                perror("send");
                running = 0;
                break;
            }

            if (strcmp(userInput, "QUIT") == 0) {

            
                running = 0;
                break;
            }
        }
    }

    // Wait for receiver thread to exit
    pthread_join(tid, NULL);

#ifdef _WIN32
    closesocket(netSocket);
    WSACleanup();
#else
    close(netSocket);
#endif

    return 0;
}