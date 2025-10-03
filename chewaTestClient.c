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
    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    netSocket = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9001);

    // Cross-platform way to safely set loopback address
#ifdef _WIN32
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);
#else
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);
#endif

    int connectStatus = connect(netSocket, (struct sockaddr*)&server_address, sizeof(server_address));

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

    printf("enter message to send to server: ");

    fgets(clientMessage, sizeof(clientMessage), stdin);
    clientMessage[strcspn(clientMessage, "\n")] = 0;

    send(netSocket, clientMessage, strlen(clientMessage) + 1, 0);
    recv(netSocket, serverResponse, sizeof(serverResponse), 0);

    printf("%s\n", serverResponse);

#ifdef _WIN32
    closesocket(netSocket);
    WSACleanup();
#else
    close(netSocket);
#endif

    return 0;
}
