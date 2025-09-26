#include <stdio.h>
#include <stdlib.h>

#include <unistd.h> 

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>



int main(){

    int net_socket;

    net_socket = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9001);
    server_address.sin_addr.s_addr = INADDR_ANY;

    int connectStatus = connect(net_socket, (struct sockaddr *) &server_address, sizeof(server_address));

    if(connectStatus < 0){
        printf("something went wrong while connecting");
        return 1;
    }

    char serverResponse[256] = {0};
    recv(net_socket, &serverResponse, sizeof(serverResponse), 0);

    printf("The message the server sent was %s\n", serverResponse);

    close(net_socket);
    return 0;
}

