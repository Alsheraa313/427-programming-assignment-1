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
static int loginStatus = 0;


// Simple struct for Active Clients. It contains their username and IP address
typedef struct {
    char username[50];
    char ip[50];
    int is_root; // 0 for false, 1 for true
} ActiveClient;

// MAX clients = 10
ActiveClient active_clients[10];

// Keeps track of how many clients are currently active on the server
int active_count = 0;

// SQLite callback for printing query results to the console.
static int callback(void* data, int argc, char** argv, char** azColName) {
    int i;
    for (i = 0; i < argc; i++) {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}


// Handles the LOGIN command from the client.
// Expected: LOGIN <username> <password>
// This command logs in the user and adds their information to the active clients list and updates the number of active users
// Returns 1 if the client successfully logged in, 0 if not
int handleLoginCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char username[50], password[50];

    
    if (sscanf(args, "%49s %49s", username, password) != 2) {
        const char* msg = "403 message format error\nUsage: LOGIN <username> <password>\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT ID, is_root FROM users WHERE user_name = ? AND password = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        const char* msg = "400 invalid command\nDatabase error while logging in.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    char response[256];

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int userID = sqlite3_column_int(stmt, 0);
        int isRoot = sqlite3_column_int(stmt, 1);

        snprintf(response, sizeof(response),
            "200 OK\n",
            username, userID, isRoot ? " [ROOT]" : "", serverPrompt);
        sqlite3_finalize(stmt);
        send(clientSocket, response, strlen(response), 0);
        //*************************************************************************************************************************
        // ADDED CODE: 
        // NOTE: Changed return type from void to int, 1 = successful login, 0 = unsuccessful login
        // Loop in main will continue if 0, stop if 1, ensuring the client is REQUIRED to login before using other commands


        // Updates the active count and adds the new client to the list of active clients
        active_count++;
        ActiveClient newClient;
        strncpy(newClient.username, username, sizeof(newClient.username) - 1);
        newClient.username[sizeof(newClient.username) - 1] = '\0';

        strncpy(newClient.ip, "192.168.1.2", sizeof(newClient.ip) - 1); // Default IP is a placeholder for the ACTUAL IP address
        newClient.username[sizeof(newClient.ip) - 1] = '\0';

        newClient.is_root = isRoot;

        active_clients[active_count - 1] = newClient;
        //*************************************************************************************************************************
        return 1;
    }
    else {
        snprintf(response, sizeof(response),
            "403 Wrong UserID or Password\n",
            serverPrompt);
        return 0;
    }
}


// Handles the SELL command from the client.
// This command allows a user to sell a certain number of Pokemon cards.
// It updates both the Pokemon_Cards table and the user's USD balance.
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
        "200 OK\nSOLD: %d %s. User's balance USD $%.2f\n%s\n",
        quantity, cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}


// Handles the BUY command from the client.
// This command allows a user to buy Pokemon cards from another user (seller).
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
    // Exclude the buyer from being their own seller
    const char* findSellerSQL =
        "SELECT owner_id, count FROM pokemon_cards "
        "WHERE card_name=? AND card_type=? AND rarity=? AND count>0 AND owner_id != ? "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, findSellerSQL, -1, &stmt, NULL) != SQLITE_OK) {
        send(clientSocket, "400 invalid command: Database error when finding seller.\n",
            strlen("400 invalid command: Database error when finding seller.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, buyerID);

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
    int buyerExists = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        buyerBalance = sqlite3_column_double(stmt, 0);
        buyerExists = 1;
    }
    sqlite3_finalize(stmt);

    // Check if buyer exists
    if (!buyerExists) {
        const char* msg = "400 invalid command: Buyer does not exist.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // If buyer cannot afford, cancel transaction
    if (buyerBalance < totalPrice) {
        const char* msg = "400 invalid command: Insufficient balance for this purchase.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // Update or delete seller's card record depending on remaining count
    const char* updateSellerSQL;
    if (sellerCount == quantity) {
        updateSellerSQL = "DELETE FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }
    else {
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

    // Get updated buyer balance
    double newBuyerBalance = 0.0;
    sqlite3_stmt* balanceStmt;
    const char* getNewBalanceSQL = "SELECT usd_balance FROM users WHERE ID=?;";
    sqlite3_prepare_v2(db, getNewBalanceSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW) {
        newBuyerBalance = sqlite3_column_double(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    // Get buyer's new total card count for this card
    int newBuyerCount = 0;
    const char* getBuyerCountSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=?;";
    sqlite3_prepare_v2(db, getBuyerCountSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    sqlite3_bind_text(balanceStmt, 2, cardName, -1, SQLITE_STATIC);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW) {
        newBuyerCount = sqlite3_column_int(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    // Format response message
    char response[256];
    snprintf(response, sizeof(response),
        "200 OK\nBOUGHT: New balance: %d %s. User USD balance $%.2f\n%s",
        newBuyerCount, cardName, newBuyerBalance, serverPrompt);

    send(clientSocket, response, strlen(response), 0);
}



// Handles the BALANCE command.
// Expected format: BALANCE <userID>
// This command checks a user's USD balance in the database and returns it to the client.
void handleBalanceCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    if (sscanf(args, "%d", &userID) != 1) {
        const char* errMsg = "403 message format error\nUsage: BALANCE <OwnerID>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt* stmt;
    const char* query = "SELECT usd_balance FROM users WHERE ID = ?;";

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
        double balance = sqlite3_column_double(stmt, 0);

        snprintf(response, sizeof(response), "200 OK\nBalance: %.2f USD\n%s", balance, serverPrompt);
    }
    else {
        snprintf(response, sizeof(response),
            "400 invalid command\nUser %d doesn't exist.\n%s", userID, serverPrompt);
    }

    // Finalize the SQL statement and send response
    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}


//Handles the LOOKUP command
// Expected format: LOOKUP <card_name> and/or <type> and/or <rarity>
// This command checks if there are any cards that matches the card the user entered, if so it returns the ID,
// Card Name, Type, Rarity, Count, and Owner of the card
void handleLookupCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char arg1[50] = "", arg2[50] = "", arg3[50] = "";

    // Trim leading whitespace
    while (*args == ' ' || *args == '\t' || *args == '\n' || *args == '\r') {
        args++;
    }

    // Parse up to three arguments
    int count = sscanf(args, "%49s %49s %49s", arg1, arg2, arg3);

    // If no arguments were provided, send format error
    if (count < 1) {
        send(clientSocket,
            "403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n",
            strlen("403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n"),
            0);
        return;
    }

    char response[4096];
    memset(response, 0, sizeof(response));

    // Build SQL query depending on how many arguments there are
    const char* baseSQL =
        "SELECT id, card_name, card_type, rarity, count, owner_id "
        "FROM pokemon_cards WHERE "
        "(card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?)";

    if (count >= 2) {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }
    if (count == 3) {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, baseSQL, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(response, sizeof(response), "500 Internal Server Error: %s\n", sqlite3_errmsg(db));
        send(clientSocket, response, strlen(response), 0);
        return;
    }

#define BIND_LIKE(index, value) do { \
        char pattern[64]; \
        snprintf(pattern, sizeof(pattern), "%%%s%%", value); \
        sqlite3_bind_text(stmt, index, pattern, -1, SQLITE_TRANSIENT); \
    } while(0)

    if (count >= 1) {
        BIND_LIKE(1, arg1); BIND_LIKE(2, arg1); BIND_LIKE(3, arg1);
    }
    if (count >= 2) {
        BIND_LIKE(4, arg2); BIND_LIKE(5, arg2); BIND_LIKE(6, arg2);
    }
    if (count == 3) {
        BIND_LIKE(7, arg3); BIND_LIKE(8, arg3); BIND_LIKE(9, arg3);
    }

    strcat(response, "200 OK\n\n");
    strcat(response, "ID     Card Name           Type       Rarity     Count     Owner\n");
    strcat(response, "--------------------------------------------------------------------\n");

    int rowsFound = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rowsFound++;

        int id = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        const unsigned char* type = sqlite3_column_text(stmt, 2);
        const unsigned char* rarity = sqlite3_column_text(stmt, 3);
        int countValue = sqlite3_column_int(stmt, 4);
        const unsigned char* owner = sqlite3_column_text(stmt, 5);

        char line[256];
        snprintf(line, sizeof(line),
            "%-6d %-18s %-10s %-10s %-9d %-10s\n",
            id, name, type, rarity, countValue, owner);

        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }

    sqlite3_finalize(stmt);

    if (rowsFound == 0) {
        snprintf(response, sizeof(response), "404 error Your search did not match any records.\n");
    }

    strncat(response, serverPrompt, sizeof(response) - strlen(response) - 1);
    send(clientSocket, response, strlen(response), 0);
}


//*******************************************
// Only root can use this command so it should return an error message if the user is not a root
// Server sends: 200 OK
// Then it lists the active users, dispalys UserID and IP
// Format: 
// The list of the active users:
// John 141.215.69.202
// root 127.0.0.1
//*******************************************
// Handles the WHO command
// Expected format: WHO
// This command (WHICH CAN ONLY BE USED BY THE ROOT USER) will display a list of the active users and the user's IP address
void handleWhoCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    char response[4096];
    memset(response, 0, sizeof(response));
    
    int response_len = snprintf(response, sizeof(response), "200 OK\nThis command has not been implemented yet.\n");

    // Add the server prompt to the end of the response
    if (strlen(serverPrompt) + response_len < sizeof(response) - 1) {
        strncat(response, serverPrompt, sizeof(response) - strlen(response) - 1);
    }

    send(clientSocket, response, strlen(response), 0);
}


//*******************************************
// Only root user can list ALL records for ALL users
// John should only return John records
//*******************************************
// Handles the LIST command.
// Expected format: LIST <userID>
// This command shows all Pokemon cards owned by a specific user.
void handleListCommand(sqlite3* db, int clientSocket, char* args, const char* serverPrompt) {
    int userID;

    if (sscanf(args, "%d", &userID) != 1) {
        const char* msg = "403 message format error\nUsage: LIST <OwnerID>\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    sqlite3_stmt* stmt;

    const char* query =
        "SELECT id, card_name, card_type, rarity, count, owner_id "
        "FROM pokemon_cards WHERE owner_id = ?;";

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        const char* msg = "400 invalid command\nDatabase error while listing cards.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // Bind userID to the SQL query
    sqlite3_bind_int(stmt, 1, userID);

    char response[4096];
    memset(response, 0, sizeof(response));

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
    // Initialize Winsock on Windows for network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // Open or create the SQLite database named "users.db"
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

    // Create "users" table if it doesn't already exist
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

    // Insert users into the table
    sql = "INSERT OR IGNORE INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES "
        "(1, 'Branch', 'Tree', 'Root', 'Root01', 100, 1),"
        "(2, 'Ms', 'Partner', 'Mary', 'Mary01', 50, 0),"
        "(3, 'Mr', 'Dude', 'John', 'John01', 200, 0),"
        "(4, 'Ms', 'Misses', 'Moe', 'Moe01', 300, 0);";

    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (insert users): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Create "pokemon_cards" table if it doesn't exist
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

    // Insert default Pokémon cards if they don't already exist
    sql = "INSERT OR IGNORE INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES "
        "('Pikachu', 'Electric', 'Common', 2, 1),"
        "('Charizard', 'Fire', 'Rare', 3, 2),"
        "('Squirtle', 'Water', 'Uncommon', 30, 2);";

    rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (insert pokemon): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Create a TCP socket for the server
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    // Set up the server address (IPv4, port 9001, listen on all interfaces)
    struct sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9001);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    // Bind socket to the specified address and port
    bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress));

    // Start listening for incoming connections (queue up to 5 clients)
    listen(serverSocket, 10);
    printf("Server is listening on port 9001\n");

    // Accept the first client connection
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

    // Buffer for receiving client messages
    char clientMessage[256] = { 0 };

const char* loginPrompt = "Enter LOGIN followed by username and password\n";
const char* serverPrompt = "Available commands: BUY, SELL, DEPOSIT, BALANCE, LIST, QUIT, SHUTDOWN, LOGIN, LOGOUT, WHO, LOOKUP\n";


while (loginStatus == 0) {
    send(clientSocket, loginPrompt, strlen(loginPrompt), 0);

    memset(clientMessage, 0, sizeof(clientMessage));
    int bytesReceived = recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
    if (bytesReceived <= 0) {
        printf("client disconnected\n");
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
        clientSocket = accept(serverSocket, NULL, NULL);
        continue;
    }

    if (strncmp(clientMessage, "LOGIN", 5) == 0) {
        handleLoginCommand(db, clientSocket, clientMessage + 6, loginPrompt);
    } else {
        const char* msg = "Enter LOGIN followed by username and password \n";
        send(clientSocket, msg, strlen(msg), 0);
    }
}

// ---------------- COMMAND LOOP (AFTER LOGIN) ----------------
while (1) {
    send(clientSocket, serverPrompt, strlen(serverPrompt), 0);
    memset(clientMessage, 0, sizeof(clientMessage));

    int bytesReceived = recv(clientSocket, clientMessage, sizeof(clientMessage), 0);
    if (bytesReceived <= 0) {
        printf("Client disconnected.\n");
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
        clientSocket = accept(serverSocket, NULL, NULL);
        loginStatus = 0; // force re-login
        continue;
    }

    printf("RECEIVED (after login): %s\n", clientMessage);

    if (strncmp(clientMessage, "BALANCE", 7) == 0) {
        handleBalanceCommand(db, clientSocket, clientMessage + 8, serverPrompt);
    } else if (strncmp(clientMessage, "LIST", 4) == 0) {
        handleListCommand(db, clientSocket, clientMessage + 5, serverPrompt);
    } else if (strncmp(clientMessage, "BUY", 3) == 0) {
        handleBuyCommand(db, clientSocket, clientMessage + 4, serverPrompt);
    } else if (strncmp(clientMessage, "SELL", 4) == 0) {
        handleSellCommand(db, clientSocket, clientMessage + 5, serverPrompt);
    } else if (strncmp(clientMessage, "WHO", 3) == 0) {
        handleWhoCommand(db, clientSocket, clientMessage + 4, serverPrompt);
    } else if (strncmp(clientMessage, "LOOKUP", 6) == 0) {
        handleLookupCommand(db, clientSocket, clientMessage + 7, serverPrompt);
    } else if (strcmp(clientMessage, "LOGOUT") == 0) {
        const char* msg = "You have been logged out.\n";
        send(clientSocket, msg, strlen(msg), 0);
        loginStatus = 0;
        break; // Return to login loop
    } else if (strcmp(clientMessage, "QUIT") == 0) {
        send(clientSocket, "Goodbye!\n", 9, 0);
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
        clientSocket = accept(serverSocket, NULL, NULL);
        loginStatus = 0;
        continue;
    } else if (strcmp(clientMessage, "SHUTDOWN") == 0) {
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
    } else {
        const char* msg = "Invalid command\n";
        send(clientSocket, msg, strlen(msg), 0);
    }


}

    // Cleanup sockets and database before exiting
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