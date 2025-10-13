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
#endif

int main() {

    int netSocket;
    char serverResponse[256] = { 0 };
    char clientMessage[256] = { 0 };

#ifdef _WIN32
    // Initialize Winsock for Windows network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // Create a TCP socket
    netSocket = socket(AF_INET, SOCK_STREAM, 0);

    // Set up the server address structure
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;        
    server_address.sin_port = htons(9001);      

#ifdef _WIN32
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);
#else
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);
#endif

    // Try connecting to the server
    int connectStatus = connect(netSocket, (struct sockaddr*)&server_address, sizeof(server_address));

    // If the connection fails, print an error and clean up
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

    // Receive the initial server message (welcome or prompt)
    recv(netSocket, serverResponse, sizeof(serverResponse), 0);
    printf("%s\n", serverResponse);

#define RESPONSE_SIZE 8192  // Large buffer to handle long responses

    // Main loop: send commands and receive responses
    while (1) {
        printf("enter message to send to server: ");

        // Read user input from console
        if (!fgets(clientMessage, sizeof(clientMessage), stdin)) break;


        clientMessage[strcspn(clientMessage, "\n")] = 0;

        // Send message to server
        send(netSocket, clientMessage, strlen(clientMessage), 0);

        // If user types "quit", exit the loop
        if (strcmp(clientMessage, "QUIT") == 0) {
            printf("Client exiting.\n");
            break;
        }

        // Clear response buffer and prepare to receive server reply
        char serverResponse[RESPONSE_SIZE];
        int totalBytes = 0;

        // Receive full response from server (can be multi-part)
        while (1) {
            int bytesReceived = recv(netSocket, serverResponse + totalBytes,
                sizeof(serverResponse) - totalBytes - 1, 0);
            if (bytesReceived <= 0) break;

            totalBytes += bytesReceived;

            // If less data than buffer limit, assume response is complete
            if (bytesReceived < sizeof(serverResponse) - totalBytes - 1) break;
        }

        serverResponse[totalBytes] = '\0';
        printf("%s\n", serverResponse);

        // Check if the server has sent a shutdown message
        if (strcmp(serverResponse, "200 OK\nServer shutting down\n") == 0) {
            printf("Client exiting due to server shutdown.\n");
            break;
        }
    }

#ifdef _WIN32
    closesocket(netSocket);
    WSACleanup();
#else
    close(netSocket);
#endif

    return 0;
}