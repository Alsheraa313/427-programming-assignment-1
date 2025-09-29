#include <stdio.h>
#include <stdlib.h>

#include <unistd.h> 

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>



int main(){

    int netSocket;

    netSocket = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9001);
    server_address.sin_addr.s_addr = INADDR_ANY;

    int connectStatus = connect(netSocket, (struct sockaddr *) &server_address, sizeof(server_address));

    if(connectStatus < 0){
        printf("something went wrong while connecting");
        return 1;
    }

    char serverResponse[256] = {0};
    recv(netSocket, &serverResponse, sizeof(serverResponse), 0);

    printf("The message the server sent was %s\n", serverResponse);

    close(netSocket);
    return 0;
}