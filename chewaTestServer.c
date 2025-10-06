#define _CRT_SECURE_NO_WARNINGS

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

// BUY Command
void handleBuyCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char cardName[50], cardType[50], rarity[50];
    double price;
    int quantity, userID;

    if (sscanf(args, "%49s %49s %49s %lf %d %d",
        cardName, cardType, rarity, &price, &quantity, &userID) != 6) {
        send(clientSocket, "Invalid BUY format. Usage: BUY <Name> <Type> <Rarity> <Price> <Quantity> <UserID>\n", 84, 0);
        return;
    }

    double cost = price * quantity;

    // Validate user
    sqlite3_stmt* stmt;
    const char* userCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "Database error.\n", 16, 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, userID);

    double balance = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance = sqlite3_column_double(stmt, 0);
    }
    else {
        sqlite3_finalize(stmt);
        send(clientSocket, "User not found.\n", 16, 0);
        return;
    }
    sqlite3_finalize(stmt);

    if (balance < cost) {
        send(clientSocket, "Insufficient funds.\n", 20, 0);
        return;
    }

    // Deduct funds
    const char* updateBalance = "UPDATE users SET usd_balance = usd_balance - ? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, cost);
    sqlite3_bind_int(stmt, 2, userID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Insert/update card
    const char* insertCard =
        "INSERT INTO pokemon_cards (card_name, card_type, rarity, price, quantity, owner_id) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(card_name, card_type, rarity, owner_id) DO UPDATE SET "
        "quantity = quantity + excluded.quantity, price = excluded.price;";

    sqlite3_prepare_v2(db, insertCard, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, price);
    sqlite3_bind_int(stmt, 5, quantity);
    sqlite3_bind_int(stmt, 6, userID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Get new balance
    double newBalance = 0.0;
    sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, userID);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        newBalance = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Get new quantity
    int newQuantity = 0;
    const char* getCard = "SELECT quantity FROM pokemon_cards WHERE card_name=? AND card_type=? AND rarity=? AND owner_id=?;";
    sqlite3_prepare_v2(db, getCard, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, userID);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        newQuantity = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    char response[256];
    snprintf(response, sizeof(response), "200 OK\nBOUGHT: New balance: %d %s. User USD balance $%.2f\n%s\n",
        newQuantity, cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}


// SELL command: attempt to sell a Pokémon card
void handleSellCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char cardName[50];
    double price;
    int quantity, userID;

    if (sscanf(args, "%49s %d %lf %d", cardName, &quantity, &price, &userID) != 4) {
        send(clientSocket, "Invalid SELL format. Usage: SELL <Name> <Quantity> <Price> <UserID>\n", 70, 0);
        return;
    }

    // Validate user
    sqlite3_stmt* stmt;
    const char* userCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, userID);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        send(clientSocket, "User not found.\n", 16, 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if card exists
    int currentQuantity = 0;
    const char* cardCheck = "SELECT quantity FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
    sqlite3_prepare_v2(db, cardCheck, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        currentQuantity = sqlite3_column_int(stmt, 0);
    }
    else {
        sqlite3_finalize(stmt);
        send(clientSocket, "Card not found for this user.\n", 30, 0);
        return;
    }
    sqlite3_finalize(stmt);

    if (currentQuantity < quantity) {
        send(clientSocket, "Not enough cards to sell.\n", 26, 0);
        return;
    }

    double proceeds = price * quantity;

    // Update card quantity or delete if 0
    if (currentQuantity - quantity > 0) {
        const char* updateCard = "UPDATE pokemon_cards SET quantity = quantity - ? WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, updateCard, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    else {
        const char* deleteCard = "DELETE FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, deleteCard, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Update balance
    const char* updateBalance = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, proceeds);
    sqlite3_bind_int(stmt, 2, userID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Get new balance
    double newBalance = 0.0;
    sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, userID);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        newBalance = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    char response[256];
    snprintf(response, sizeof(response), "200 OK\nSOLD: New balance: %d %s. User’s balance USD $%.2f\n%s\n",
        (currentQuantity - quantity > 0 ? currentQuantity - quantity : 0),
        cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}


// LIST command: list all cards owned by a user
void handleListCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    if (sscanf(args, "%d", &userID) != 1) {
        send(clientSocket, "Invalid LIST format. Usage: LIST <OwnerID>\n",
            strlen("Invalid LIST format. Usage: LIST <OwnerID>\n"), 0);
        return;
    }

    sqlite3_stmt* stmt;
    const char* query = "SELECT card_name, card_type, rarity, count "
        "FROM pokemon_cards WHERE owner_id = ?;";

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "Database error (LIST).\n",
            strlen("Database error (LIST).\n"), 0);
        return;
    }

    sqlite3_bind_int(stmt, 1, userID);

    char response[2048];
    int offset = snprintf(response, sizeof(response), "Cards owned:\n");
    int found = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* cardName = (const char*)sqlite3_column_text(stmt, 0);
        const char* cardType = (const char*)sqlite3_column_text(stmt, 1);
        const char* rarity = (const char*)sqlite3_column_text(stmt, 2);
        int count = sqlite3_column_int(stmt, 3);

        offset += snprintf(response + offset, sizeof(response) - offset,
            "%s (%s, %s) - %d owned\n",
            cardName, cardType, rarity, count);
        found = 1;
    }

    sqlite3_finalize(stmt);

    if (!found) {
        offset = snprintf(response, sizeof(response), "No cards found for user %d.\n", userID);
    }

    snprintf(response + offset, sizeof(response) - offset, "%s", serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}


// BALANCE command: get a user's USD balance
void handleBalanceCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    // Expected format: BALANCE <userID>
    if (sscanf(args, "%d", &userID) != 1) {
        send(clientSocket, "Invalid BALANCE format. Usage: BALANCE <OwnerID>\n",
            strlen("Invalid BALANCE format. Usage: BALANCE <OwnerID>\n"), 0);
        return;
    }

    sqlite3_stmt* stmt;
    const char* query = "SELECT usd_balance FROM users WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "Database error (BALANCE).\n",
            strlen("Database error (BALANCE).\n"), 0);
        return;
    }

    sqlite3_bind_int(stmt, 1, userID);

    char response[256];
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        double balance = sqlite3_column_double(stmt, 0);
        snprintf(response, sizeof(response), "Balance: %.2f USD\n%s", balance, serverPrompt);
    }
    else {
        snprintf(response, sizeof(response), "Error: User ID %d not found.\n%s", userID, serverPrompt);
    }

    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
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

    // Create pokemon_cards table
    const char* create_pokemon_cards_table =
        "CREATE TABLE IF NOT EXISTS pokemon_cards ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "card_name TEXT NOT NULL, "
        "card_type TEXT NOT NULL, "
        "rarity TEXT NOT NULL, "
        "count INTEGER NOT NULL, "
        "owner_id INTEGER NOT NULL, "
        "FOREIGN KEY(owner_id) REFERENCES users(ID), "
        "UNIQUE(card_name, card_type, rarity, owner_id)"
        ");";

    rc = sqlite3_exec(db, create_pokemon_cards_table, callback, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create pokemon table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Insert sample pokemon if table is empty
    sql = "INSERT OR IGNORE INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES "
        "('Pikachu', 'Electric', 'Common', 2, 1),"
        "('Charizard', 'Fire', 'Rare', 3, 2),"
        "('Squirtle', 'Water', 'Uncommon', 30, 2);";
    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (insert pokemon): %s\n", zErrMsg);
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

        if (strncmp(clientMessage, "BALANCE", 7) == 0) {
            handleBalanceCommand(db, clientSocket, clientMessage + 8, serverPrompt);
        }
        else if (strncmp(clientMessage, "LIST", 4) == 0) {
            handleListCommand(db, clientSocket, clientMessage + 5, serverPrompt);
        }
        else if (strncmp(clientMessage, "BUY", 3) == 0) {
            handleBuyCommand(db, clientSocket, clientMessage + 4, serverPrompt);
        }
        else if (strncmp(clientMessage, "SELL", 4) == 0) {
            handleSellCommand(db, clientSocket, clientMessage + 5, serverPrompt);
        }
        else if (strcmp(clientMessage, "quit") == 0) {
            send(clientSocket, "Goodbye!\n", 10, 0);
            #ifdef _WIN32
            closesocket(clientSocket);
            #else
            close(clientSocket);
            #endif
            // Go back to listening for new clients instead of breaking the server loop
            clientSocket = accept(serverSocket, NULL, NULL);
            if (clientSocket < 0) {
                perror("Failed to accept new client");
                continue; // server stays alive
            }
            send(clientSocket, serverPrompt, strlen(serverPrompt) + 1, 0);
        }
        else if (strcmp(clientMessage, "shutdown") == 0) {
            send(clientSocket, "Server shutting down\n", 22, 0);
            #ifdef _WIN32
            closesocket(clientSocket);
            closesocket(serverSocket);
            WSACleanup();
            #else
            close(clientSocket);
            close(serverSocket);
            #endif
            sqlite3_close(db);
            exit(0); // terminate server
        }
        else {
            send(clientSocket, "Invalid command", 15, 0);
        }
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
