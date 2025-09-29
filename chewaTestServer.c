#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h> 

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {

    char clientMessage[256] = {0};
    const char *serverMessage = "hello i am server";
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9001);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)); 
     
    listen(serverSocket, 5);

    printf("server is listening\n");

    int clientSocket = accept(serverSocket, NULL, NULL);
    if (clientSocket < 0) {
        perror("connection failed");
        close(serverSocket);
        exit(1);
    }


recv(clientSocket, clientMessage, sizeof(clientMessage), 0);

if (strcmp(clientMessage, "buy") == 0) {

send(clientSocket, "input recived was 'buy'", strlen("input recived was buy"), 0);

    /*
    here ask for which card they want to buy, check if its in the array, if its valid, pop it outta the array
    */

}

else if (strcmp(clientMessage, "sell") == 0) {

send(clientSocket, "input received was 'sell'", strlen("input received was 'sell'"), 0);
    /*
    here just add whatever they type into a constructor that makes the card like 
    "what would you like to sell(name, price, amount)?: 
    and the input would be like 
    pikachu, 10, 1
    doesnt have to be too sophisticated 
    
    */

}

    close(clientSocket);
    close(serverSocket);

    return 0;
}
