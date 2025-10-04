#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "sqlite3.h"
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#endif

// ---------------------
// SQLite callback for printing query results
static int callback(void* data, int argc, char** argv, char** azColName) {
    int i;
    for (i = 0; i < argc; i++) {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}

// Callback for balance queries
static int balanceCallback(void* data, int argc, char** argv, char** azColName) {
    if (argc > 0 && argv[0]) {
        double* balance = (double*)data;
        *balance = atof(argv[0]);
    }
    return 0;
}

// Get user's USD balance
double getBalance(sqlite3* db, int ID) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT usd_balance FROM users WHERE ID=%d;", ID);

    double balance = 0.0;
    char* zErrMsg = 0;
    int rc = sqlite3_exec(db, sql, balanceCallback, &balance, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (get_balance): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return 0.0;
    }
    return balance;
}

int main() {

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // ---------------------
    // Setup SQLite database
    sqlite3* db;
    char* zErrMsg = 0;
    int rc = sqlite3_open("users.db", &db);  // create users.db in project folder
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    fprintf(stderr, "Opened database successfully\n");

    // Create Users table if it doesn't exist
    char* sql = "CREATE TABLE IF NOT EXISTS users ("
        "ID INTEGER PRIMARY KEY,"
        "first_name TEXT,"
        "last_name TEXT,"
        "user_name TEXT NOT NULL,"
        "password TEXT,"
        "usd_balance DOUBLE NOT NULL,"
        "is_root INTEGER NOT NULL DEFAULT 0);";

    rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Insert sample users if table is empty
    sql = "INSERT OR IGNORE INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES "
        "(1, 'chewa', 'shewa', 'shewashewa', 'passwerd', 100, 1),"
        "(2, 'mr', 'partner', 'mr.partner', 'pass', 50, 0);";
    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (insert users): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // ---------------------
    // Setup TCP server
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9001);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress));
    listen(serverSocket, 5);
    printf("Server is listening on port 9001\n");

    int clientSocket = accept(serverSocket, NULL, NULL);
    if (clientSocket < 0) {
        perror("Connection failed");
#ifdef _WIN32
        closesocket(serverSocket);
        WSACleanup();
#else
        close(serverSocket);
#endif
        sqlite3_close(db);
        return 1;
    }

    char clientMessage[256] = { 0 };
    const char* serverPrompt = "Available commands: BUY, SELL, LIST, BALANCE, QUIT, SHUTDOWN";
    send(clientSocket, serverPrompt, strlen(serverPrompt) + 1, 0);

    // ---------------------
    // Main server loop
    while (1) {
        memset(clientMessage, 0, sizeof(clientMessage));
        recv(clientSocket, clientMessage, sizeof(clientMessage), 0);

        if (strcmp(clientMessage, "balance") == 0) {
            send(clientSocket, "Enter owner ID to see balance:", 33, 0);
            recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
            int ownerID = atoi(clientMessage);
            double balance = getBalance(db, ownerID);
            char balanceResponse[256];
            snprintf(balanceResponse, sizeof(balanceResponse), "Balance: %.2f USD", balance);
            send(clientSocket, balanceResponse, strlen(balanceResponse) + 1, 0);
        }
        else if (strcmp(clientMessage, "list") == 0) {
            send(clientSocket, "LIST command received (to be implemented)", 42, 0);
        }
        else if (strcmp(clientMessage, "buy") == 0) {
            send(clientSocket, "BUY command received (to be implemented)", 40, 0);
        }
        else if (strcmp(clientMessage, "sell") == 0) {
            send(clientSocket, "SELL command received (to be implemented)", 41, 0);
        }
        else if (strcmp(clientMessage, "quit") == 0) {
            break;
        }
        else if (strcmp(clientMessage, "shutdown") == 0) {
            send(clientSocket, "Server shutting down", 21, 0);
            break;
        }
        else {
            send(clientSocket, "Invalid command", 15, 0);
        }

        send(clientSocket, serverPrompt, strlen(serverPrompt) + 1, 0);
    }

#ifdef _WIN32
    closesocket(clientSocket);
    closesocket(serverSocket);
    WSACleanup();
#else
    close(clientSocket);
    close(serverSocket);
#endif

    sqlite3_close(db);
    return 0;
}
