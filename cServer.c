#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h> 

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sqlite3.h>


static int balanceCallback(void *data, int argc, char **argv, char **azColName) {
    if (argc > 0 && argv[0]) {
        double *balance = (double*)data;
        *balance = atof(argv[0]);
    }
    return 0;
}

double getBalance(sqlite3 *users, int ID) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT usd_balance FROM users WHERE ID=%d;", ID);

    double balance = 0.0;
    char *zErrMsg = 0;

    int rc = sqlite3_exec(users, sql, balanceCallback, &balance, &zErrMsg);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (get_balance): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return 1;
    }
    return balance;
}

int main() {
  sqlite3 *users;
    char *zErrMsg = 0;
    int rc;

    rc = sqlite3_open("users", &users);
    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(users));
        return 1;
    }
    fprintf(stderr, "Opened database successfully\n");

    char *sql = "CREATE TABLE IF NOT EXISTS users ("
                "ID INTEGER PRIMARY KEY,"
                "first_name TEXT,"
                "last_name TEXT,"
                "user_name TEXT NOT NULL,"
                "password TEXT,"
                "usd_balance DOUBLE NOT NULL,"
                "is_root INTEGER NOT NULL DEFAULT 0)";

    rc = sqlite3_exec(users, sql, 0, 0, &zErrMsg);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    sql = "INSERT INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES"
          "(1, 'chewa', 'shewa', 'shewashewa', 'passwerd', 100, 1),"
          "(2, 'mr', 'partner', 'mr.partner', 'pass', 50, 0)";

    rc = sqlite3_exec(users, sql, 0, 0, &zErrMsg);

    char clientMessage[256] = {0};
    const char *serverMessage = "please input: buy (ownerID), sell buy(ownerID), list buy(ownerID), balance buy(ownerID), shutdown/quit";
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



send(clientSocket, serverMessage,  strlen(serverMessage) + 1, 0);

while(1){
    

recv(clientSocket, clientMessage, sizeof(clientMessage), 0);

if(strcmp(clientMessage, "balance") == 0){


 send(clientSocket, "enter the ownerID youd like to see the balance of", 
    strlen("enter the ownerID youd like to see the balance of") + 1, 0);

    recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
    int ownerID = atoi(clientMessage);
    double balance = getBalance(users, ownerID);
    char balanceResponse[256];
    snprintf(balanceResponse, sizeof(balanceResponse), "balance: %.2f", balance);
    send(clientSocket, balanceResponse, strlen(balanceResponse) + 1, 0);

}
else if(strcmp(clientMessage, "list") == 0){
    send(clientSocket, "enter the ownerID youd like to see the invetory of", strlen("enter the ownerID youd like to see the inventory of") + 1, 0);
    recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
    int ownerID = atoi(clientMessage);
}
else if (strcmp(clientMessage, "buy") == 0) {
    send(clientSocket, "input recived was 'buy'", strlen("input recived was 'buy'") + 1, 0);
}
else if (strcmp(clientMessage, "sell") == 0) {
    send(clientSocket, "input received was 'sell'", strlen("input received was 'sell'") + 1, 0);
}
else if(strcmp(clientMessage, "quit") == 0){
    break;
}
else if(strcmp(clientMessage, "shutdown") == 0){
    close(clientSocket);
    close(serverSocket);
    exit(0);
}

send(clientSocket, serverMessage,  strlen(serverMessage) + 1, 0);
}
    close(clientSocket);
    close(serverSocket);

    return 0;
}