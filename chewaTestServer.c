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

// SQLite callback for printing query results to the console.
// This function is used when running general SQL queries for testing or debugging.
static int callback(void* data, int argc, char** argv, char** azColName) {
    int i;
    // Loop through all columns returned by the SQL query
    for (i = 0; i < argc; i++) {
        // Print each column name and its value, or "NULL" if no value exists
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0; // Returning 0 tells SQLite to continue
}


// SQLite callback specifically for extracting balance values from a query.
// The balance value is stored in the variable pointed to by 'data'.
static int balanceCallback(void* data, int argc, char** argv, char** azColName) {
    // Check if the query returned a value
    if (argc > 0 && argv[0]) {
        double* balance = (double*)data; // Cast void pointer to double pointer
        *balance = atof(argv[0]); // Convert string result to double and store it
    }
    return 0; // Continue normal execution
}


// Retrieves the USD balance for a given user ID from the database.
double getBalance(sqlite3* db, int ID) {
    char sql[256];
    // Create SQL query string to fetch the user's balance
    snprintf(sql, sizeof(sql), "SELECT usd_balance FROM users WHERE ID=%d;", ID);

    double balance = 0.0;
    char* zErrMsg = 0;

    // Execute the SQL query, passing balanceCallback to extract the result
    int rc = sqlite3_exec(db, sql, balanceCallback, &balance, &zErrMsg);

    // If the SQL query fails, print the error and return 0
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (get_balance): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return 0.0;
    }

    // Return the user's balance
    return balance;
}


// Handles the SELL command from the client.
// This command allows a user to sell a certain number of Pokémon cards.
// It updates both the Pokémon_Cards table and the user's USD balance.
void handleSellCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char cardName[50];
    int quantity, userID;
    double price;

    // Expected format: SELL <cardName> <quantity> <price> <userID>
    // If the format is wrong, return a 403 message format error.
    if (sscanf(args, "%49s %d %lf %d", cardName, &quantity, &price, &userID) != 4) {
        send(clientSocket, "403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n",
            strlen("403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n"), 0);
        return;
    }

    sqlite3_stmt* stmt;

    // Check if the user exists
    const char* userCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "400 invalid command: Database error.\n",
            strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, userID);

    double currentBalance = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        currentBalance = sqlite3_column_double(stmt, 0);
    }
    else {
        // User ID not found in the database
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: User does not exist.\n",
            strlen("400 invalid command: User does not exist.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if the user owns the card and how many they have
    int currentQuantity = 0;
    const char* cardCheck = "SELECT count FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
    if (sqlite3_prepare_v2(db, cardCheck, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "400 invalid command: Database error.\n",
            strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        currentQuantity = sqlite3_column_int(stmt, 0);
    }
    else {
        // The specified card was not found for this user
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: Card not found for this user.\n",
            strlen("400 invalid command: Card not found for this user.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if user has enough cards to sell
    if (currentQuantity < quantity) {
        send(clientSocket, "400 invalid command: Not enough cards to sell.\n",
            strlen("400 invalid command: Not enough cards to sell.\n"), 0);
        return;
    }

    // If user still has cards left after selling, update the count
    if (currentQuantity - quantity > 0) {
        const char* updateCard = "UPDATE pokemon_cards SET count=count-? WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, updateCard, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Otherwise, delete the card entry since the user sold them all
    else {
        const char* deleteCard = "DELETE FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, deleteCard, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Calculate total money earned from selling the cards
    double proceeds = price * quantity;

    // Add proceeds to the user's balance
    const char* updateBalance = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, proceeds);
    sqlite3_bind_int(stmt, 2, userID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Compute new balance manually for confirmation
    double newBalance = currentBalance + proceeds;

    // Send a success message back to the client
    char response[256];
    snprintf(response, sizeof(response),
        "200 OK\nSOLD: %d %s. User’s balance USD $%.2f\n%s\n",
        quantity, cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}


// Handles the BUY command from the client.
// This command allows a user to buy Pokémon cards from another user (seller).
// It checks balances, updates both users' balances, and updates the card counts.
void handleBuyCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char cardName[50], cardType[50], rarity[50];
    double price;
    int quantity, buyerID;

    // Expected format: BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>
    // If the input format is wrong, send a 403 format error message.
    if (sscanf(args, "%49s %49s %49s %lf %d %d", cardName, cardType, rarity, &price, &quantity, &buyerID) != 6) {
        const char* usageMsg = "403 message format error: Usage -> BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>\n";
        send(clientSocket, usageMsg, strlen(usageMsg), 0);
        return;
    }

    sqlite3_stmt* stmt;

    // Find a seller who has at least one of the requested card
    const char* findSellerSQL = "SELECT owner_id, count FROM pokemon_cards WHERE card_name=? AND card_type=? AND rarity=? AND count>0 LIMIT 1;";
    if (sqlite3_prepare_v2(db, findSellerSQL, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "400 invalid command: Database error when finding seller.\n",
            strlen("400 invalid command: Database error when finding seller.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);

    int sellerID = -1;
    int sellerCount = 0;

    // If seller found, store their ID and how many cards they have
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sellerID = sqlite3_column_int(stmt, 0);
        sellerCount = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    // If no seller found, send an error message
    if (sellerID == -1) {
        send(clientSocket, "400 invalid command: No seller currently offering that card.\n",
            strlen("400 invalid command: No seller currently offering that card.\n"), 0);
        return;
    }

    // Prevent a user from buying their own card
    if (sellerID == buyerID) {
        send(clientSocket, "400 invalid command: You cannot buy a card from yourself.\n",
            strlen("400 invalid command: You cannot buy a card from yourself.\n"), 0);
        return;
    }

    // Check if seller has enough cards to fulfill the request
    if (sellerCount < quantity) {
        send(clientSocket, "400 invalid command: Seller does not have enough of that card.\n",
            strlen("400 invalid command: Seller does not have enough of that card.\n"), 0);
        return;
    }
    
    // Calculate total cost of the transaction
    double totalPrice = price * quantity;

    // Check buyer's balance first
    const char* checkBalanceSQL = "SELECT usd_balance FROM users WHERE ID=?;";
    sqlite3_prepare_v2(db, checkBalanceSQL, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, buyerID);

    double buyerBalance = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        buyerBalance = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // If buyer cannot afford, cancel transaction
    if (buyerBalance < totalPrice) {
        const char* msg = "400 invalid command: Insufficient balance for this purchase.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }


    // Update or delete seller's card record depending on remaining count
    const char* updateSellerSQL;
    if (sellerCount == quantity) {
        // Delete card entry if all cards are sold
        updateSellerSQL = "DELETE FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }
    else {
        // Subtract sold cards from seller’s count
        updateSellerSQL = "UPDATE pokemon_cards SET count=count-? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }

    if (sqlite3_prepare_v2(db, updateSellerSQL, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "400 invalid command: Database error when updating seller.\n",
            strlen("400 invalid command: Database error when updating seller.\n"), 0);
        return;
    }

    // Bind values depending on which SQL query was chosen
    if (sellerCount == quantity) {
        sqlite3_bind_int(stmt, 1, sellerID);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, rarity, -1, SQLITE_STATIC);
    }
    else {
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_int(stmt, 2, sellerID);
        sqlite3_bind_text(stmt, 3, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, rarity, -1, SQLITE_STATIC);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Check if the buyer already owns the same type of card
    const char* checkBuyerSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    sqlite3_prepare_v2(db, checkBuyerSQL, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, buyerID);
    sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, rarity, -1, SQLITE_STATIC);

    int buyerCount = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        buyerCount = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // If buyer already owns the card, increase their count
    if (buyerCount > 0) {
        const char* updateBuyerSQL = "UPDATE pokemon_cards SET count=count+? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
        sqlite3_prepare_v2(db, updateBuyerSQL, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_int(stmt, 2, buyerID);
        sqlite3_bind_text(stmt, 3, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, rarity, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Otherwise, insert a new record for the buyer
    else {
        const char* insertBuyerSQL = "INSERT INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES (?, ?, ?, ?, ?);";
        sqlite3_prepare_v2(db, insertBuyerSQL, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, quantity);
        sqlite3_bind_int(stmt, 5, buyerID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Calculate total cost of the transaction
    totalPrice = price * quantity;

    // Deduct total price from buyer's balance
    const char* updateBuyerBalance = "UPDATE users SET usd_balance=usd_balance-? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBuyerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, buyerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Add total price to seller's balance
    const char* updateSellerBalance = "UPDATE users SET usd_balance=usd_balance+? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateSellerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, sellerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Send success response back to the client
    char response[256];
    snprintf(response, sizeof(response),
        "200 OK\nBOUGHT: %d × %s (%s, %s) for %.2f USD total.\n%s",
        quantity, cardName, cardType, rarity, totalPrice, serverPrompt);

    send(clientSocket, response, strlen(response), 0);
}


// Handles the BALANCE command.
// Expected format: BALANCE <userID>
// This command checks a user's USD balance in the database and returns it to the client.
void handleBalanceCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    // Parse arguments from the client command
    // If not formatted correctly, return a 403 message format error.
    if (sscanf(args, "%d", &userID) != 1) {
        const char* errMsg = "403 message format error\nUsage: BALANCE <OwnerID>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt* stmt;
    const char* query = "SELECT usd_balance FROM users WHERE ID = ?;";

    // Prepare SQL query to check the user's balance
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        const char* errMsg = "400 invalid command\nDatabase error while checking balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    // Bind user ID value into the prepared SQL statement
    sqlite3_bind_int(stmt, 1, userID);

    char response[256];

    // Execute the query and check if a record is found
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Extract balance value from query result
        double balance = sqlite3_column_double(stmt, 0);

        // Send success response to the client
        snprintf(response, sizeof(response), "200 OK\nBalance: %.2f USD\n%s", balance, serverPrompt);
    }
    else {
        // User ID not found, return 400 invalid command with explanation
        snprintf(response, sizeof(response),
            "400 invalid command\nUser %d doesn’t exist.\n%s", userID, serverPrompt);
    }

    // Finalize the SQL statement and send response
    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}


// Handles the LIST command.
// Expected format: LIST <userID>
// This command shows all Pokemon cards owned by a specific user.
void handleListCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    // Check if user entered the command in the right format.
    // If not, return a 403 message format error.
    if (sscanf(args, "%d", &userID) != 1) {
        const char* msg = "403 message format error\nUsage: LIST <OwnerID>\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    sqlite3_stmt* stmt;

    // SQL query to get all Pokemon cards owned by the given user
    const char* query =
        "SELECT id, card_name, card_type, rarity, count, owner_id "
        "FROM pokemon_cards WHERE owner_id = ?;";

    // Prepare the SQL statement
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        const char* msg = "400 invalid command\nDatabase error while listing cards.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // Bind userID to the SQL query
    sqlite3_bind_int(stmt, 1, userID);

    char response[4096];
    memset(response, 0, sizeof(response));

    // Start building the success response message
    int response_len = snprintf(response, sizeof(response),
        "200 OK\nThe list of records in the Pokemon cards table for user %d:\n"
        "---------------------------------------------------------------\n"
        "ID   Card Name        Type        Rarity      Count  OwnerID\n",
        userID);

    int found = 0; // Tracks if any cards were found

    // Loop through each result row and format card info
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char* cardName = (const char*)sqlite3_column_text(stmt, 1);
        const char* cardType = (const char*)sqlite3_column_text(stmt, 2);
        const char* rarity = (const char*)sqlite3_column_text(stmt, 3);
        int count = sqlite3_column_int(stmt, 4);
        int owner = sqlite3_column_int(stmt, 5);

        // Append the card info to the response string
        int written = snprintf(response + response_len, sizeof(response) - response_len,
            "%-4d %-15s %-11s %-11s %-6d %-6d\n",
            id, cardName, cardType, rarity, count, owner);

        // Stop writing if we run out of buffer space
        if (written < 0 || written >= (int)(sizeof(response) - response_len - 1))
            break;

        response_len += written;
        found = 1;
    }
    sqlite3_finalize(stmt);

    // If the user has no cards, return a message stating that
    if (!found) {
        response_len = snprintf(response, sizeof(response),
            "200 OK\nNo cards found for user %d.\n", userID);
    }

    // Add the server prompt to the end of the response
    if (strlen(serverPrompt) + response_len < sizeof(response) - 1) {
        strncat(response, serverPrompt, sizeof(response) - strlen(response) - 1);
    }

    // Send final formatted response to the client
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

    // Open or create the SQLite database
    sqlite3* db;
    char* zErrMsg = 0;
    int rc = sqlite3_open("users.db", &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    fprintf(stderr, "Opened database successfully\n");

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

sql = "INSERT OR IGNORE INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES "
    "(1, 'chewa', 'shewa', 'shewashewa', 'passwerd', 100, 1),"
    "(2, 'mr', 'partner', 'mr.partner', 'pass', 50, 0);";
rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error (insert users): %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
}

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

rc = sqlite3_exec(db, create_pokemon_cards_table, NULL, NULL, &zErrMsg);
if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error (create pokemon table): %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
}

sql = "INSERT OR IGNORE INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES "
    "('Pikachu', 'Electric', 'Common', 2, 1),"
    "('Charizard', 'Fire', 'Rare', 3, 2),"
    "('Squirtle', 'Water', 'Uncommon', 30, 2);";

rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error (insert pokemon): %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
}

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
    int bytesReceived = recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
    if (bytesReceived <= 0) {
        printf("Client disconnected or error occurred.\n");
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
        clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket < 0) {
            perror("Failed to accept new client");
            continue;
        }
        send(clientSocket, serverPrompt, strlen(serverPrompt) + 1, 0);
        continue;
    }

    // Print all messages received from clients
    printf("RECEIVED: %s\n", clientMessage);

    // Handle commands
    if (strncmp(clientMessage, "BALANCE", 7) == 0) {
        handleBalanceCommand(db, clientSocket, clientMessage + 8, serverPrompt);
    }
    else if (strncmp(clientMessage, "LIST", 4) == 0) {
        handleListCommand(db, clientSocket, clientMessage + 5, serverPrompt);
    }
    else if (strncmp(clientMessage, "BUY", 3) == 0) {
        char* args = clientMessage + 4;
        handleBuyCommand(db, clientSocket, args, serverPrompt);
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
        clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket < 0) {
            perror("Failed to accept new client");
            continue;
        }
        send(clientSocket, serverPrompt, strlen(serverPrompt) + 1, 0);
    }
    else if (strcmp(clientMessage, "shutdown") == 0) {
        const char* okMsg = "200 OK\nServer shutting down\n";
        send(clientSocket, okMsg, strlen(okMsg), 0);
#ifdef _WIN32
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
#else
        close(clientSocket);
        close(serverSocket);
#endif

        sqlite3_close(db);
        printf("Server terminated by shutdown command.\n");
        exit(0);
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
