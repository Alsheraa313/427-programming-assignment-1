#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#ifdef _WIN32
// --- POSIX → Windows mappings ---
#define pthread_cond_t HANDLE
#define pthread_cond_init(cond, attr) (*(cond) = CreateEvent(NULL, FALSE, FALSE, NULL))
#define pthread_cond_signal(cond) SetEvent(*(cond))
#define pthread_cond_wait(cond, mutex) WaitForSingleObject(*(cond), INFINITE)

#define pthread_mutex_lock(mutex) WaitForSingleObject(*(mutex), INFINITE)
#define pthread_mutex_unlock(mutex) ReleaseMutex(*(mutex))
#define pthread_mutex_init(mutex, attr) (*(mutex) = CreateMutex(NULL, FALSE, NULL))
#define pthread_mutex_destroy(mutex) CloseHandle(*(mutex))

#define STRCMP_NOCASE _stricmp
#else
#include <pthread.h>
#include <strings.h>
#define STRCMP_NOCASE strcasecmp
#endif


#ifdef _WIN32
#include "sqlite3.h"
#else
#include <sqlite3.h>
#endif


// --- Platform-specific includes ---
#ifdef _WIN32
    // Windows sockets & threading
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

// Make POSIX-style names work cross-platform
#define close closesocket
#define sleep(x) Sleep(1000 * (x))

// Thread synchronization
typedef HANDLE thread_mutex_t;
typedef HANDLE thread_cond_t;

#else   // ---------- Linux / macOS ----------
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

typedef pthread_mutex_t thread_mutex_t;
typedef pthread_cond_t  thread_cond_t;
#endif

// --- Case-insensitive string compare ---
#ifdef _WIN32
#define STRCMP_NOCASE _stricmp
#else
#include <strings.h>
#define STRCMP_NOCASE strcasecmp
#endif

// --- Constants ---
#define MAX_TASKS 100
#define NUM_WORKERS 4

// --- Globals ---
sqlite3* db;
thread_mutex_t db_mutex;
thread_mutex_t active_clients_mutex;

int client_sockets[FD_SETSIZE];
int serverSocket;
int login_status[FD_SETSIZE]; // 0 = not logged in, 1 = logged in
char username_by_slot[FD_SETSIZE][50];
int conn_count = 0;
fd_set master_set;



// --- Windows equivalents of pthreads ---
typedef CRITICAL_SECTION pthread_mutex_t;

#define pthread_mutex_init(m, a) InitializeCriticalSection(m)
#define pthread_mutex_lock(m) EnterCriticalSection(m)
#define pthread_mutex_unlock(m) LeaveCriticalSection(m)
#define pthread_mutex_destroy(m) DeleteCriticalSection(m)

// Initialize the mutexes like pthread style
thread_mutex_t active_clients_mutex;
thread_mutex_t db_mutex;

sqlite3 *db;

static void init_mutexes()
{
    pthread_mutex_init(&active_clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);
}

static void destroy_mutexes()
{
    pthread_mutex_destroy(&active_clients_mutex);
    pthread_mutex_destroy(&db_mutex);
}

typedef struct
{
    int client_socket;
    char message[512];
} Task;

typedef struct
{
    Task tasks[MAX_TASKS];
    int front, rear, count;

#ifdef _WIN32
    HANDLE mutex;
    HANDLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif

} TaskQueue;

#ifdef _WIN32
HANDLE cond;
#else
pthread_cond_t cond;
#endif


TaskQueue workQueue;

// --- Task Queue Functions ---
void initQueue(TaskQueue *q)
{
    q->front = q->rear = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void enqueue(TaskQueue *q, Task t)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == MAX_TASKS)
        pthread_cond_wait(&q->cond, &q->mutex);
    q->tasks[q->rear] = t;
    q->rear = (q->rear + 1) % MAX_TASKS;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

Task dequeue(TaskQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0)
        pthread_cond_wait(&q->cond, &q->mutex);
    Task t = q->tasks[q->front];
    q->front = (q->front + 1) % MAX_TASKS;
    q->count--;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return t;
}

sqlite3* db;

#ifdef _WIN32
HANDLE active_clients_mutex;
HANDLE db_mutex;
#else
pthread_mutex_t active_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// struct that holds client info of whoever is logged in
typedef struct
{
    char username[50];
    char ip[50];
    int is_root;
} ActiveClient;

ActiveClient active_clients[10];

// this gets incremented whenever another client joins
int active_count = 0;

// standard sqlite callback function
static int callback(void *data, int argc, char **argv, char **azColName)
{
    int i;
    (void)data; /* unused */
    for (i = 0; i < argc; i++)
    {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}

/*login function, checks input first and then capacity, after which itll pull the user from the sql db, copy it and
add it to the activeClient array and increment the count. we had to move the menu here */
/*if a user isnt found in the db, sends out an error*/
// Expected: LOGIN <username> <password>
int handleLoginCommand(sqlite3 *db, int clientSocket, char *args)
{
    char username[50], password[50];

    if (sscanf(args, "%49s %49s", username, password) != 2)
    {
        const char *msg = "403 syntax error: LOGIN <username> <password>\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    if (active_count >= 10)
    {
        const char *msg = "503 server is full\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT ID, is_root FROM users WHERE user_name = ? AND password = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    char response[256];

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int isRoot = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);

        ActiveClient newClient;
        memset(&newClient, 0, sizeof(newClient));

        strncpy(newClient.username, username, sizeof(newClient.username) - 1);
        newClient.is_root = isRoot;

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(clientSocket, (struct sockaddr *)&addr, &addr_len) == 0)
        {
            inet_ntop(AF_INET, &addr.sin_addr, newClient.ip, sizeof(newClient.ip));
        }
        else
        {
            // Fallback to localhost if lookup fails
            strncpy(newClient.ip, "127.0.0.1", sizeof(newClient.ip) - 1);
            newClient.ip[sizeof(newClient.ip) - 1] = '\0';
        }

        // --- Store active client ---
        active_clients[active_count++] = newClient;

        printf("User '%s' logged in from %s | Active clients: %d\n",
               newClient.username, newClient.ip, active_count);

        const char *successMsg =
            "200 OK\n"
            "Login successful!\n"
            "Available commands: BUY, SELL, BALANCE, DEPOSIT, LIST, QUIT, LOGOUT, WHO, LOOKUP\nEnter message to send to server: ";
        send(clientSocket, successMsg, strlen(successMsg), 0);

        return 1;
    }
    else
    {
        sqlite3_finalize(stmt);
        snprintf(response, sizeof(response), "403 Wrong UserID or Password\n");
        send(clientSocket, response, strlen(response), 0);
        return 0;
    }
}

// Handles the SELL command from the client.
// This command allows a user to sell a certain number of Pokemon cards.
// It updates both the Pokemon_Cards table and the user's USD balance.
void handleSellCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    char cardName[50];
    int quantity, userID;
    double price;

    // Expected format: SELL <cardName> <quantity> <price> <userID>
    // If the format is wrong, return a 403 message format error.
    if (sscanf(args, "%49s %d %lf %d", cardName, &quantity, &price, &userID) != 4)
    {
        send(clientSocket, "403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n",
             strlen("403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n"), 0);
        return;
    }

    sqlite3_stmt *stmt;

    // Check if the user exists
    const char *userCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error.\n",
             strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, userID);

    double currentBalance = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        currentBalance = sqlite3_column_double(stmt, 0);
    }
    else
    {
        // User ID not found in the database
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: User does not exist.\n",
             strlen("400 invalid command: User does not exist.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if the user owns the card and how many they have
    int currentQuantity = 0;
    const char *cardCheck = "SELECT count FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
    if (sqlite3_prepare_v2(db, cardCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error.\n",
             strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        currentQuantity = sqlite3_column_int(stmt, 0);
    }
    else
    {
        // The specified card was not found for this user
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: Card not found for this user.\n",
             strlen("400 invalid command: Card not found for this user.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if user has enough cards to sell
    if (currentQuantity < quantity)
    {
        send(clientSocket, "400 invalid command: Not enough cards to sell.\n",
             strlen("400 invalid command: Not enough cards to sell.\n"), 0);
        return;
    }

    // If user still has cards left after selling, update the count
    if (currentQuantity - quantity > 0)
    {
        const char *updateCard = "UPDATE pokemon_cards SET count=count-? WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, updateCard, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Otherwise, delete the card entry since the user sold them all
    else
    {
        const char *deleteCard = "DELETE FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, deleteCard, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Calculate total money earned from selling the cards
    double proceeds = price * quantity;

    // Add proceeds to the user's balance
    const char *updateBalance = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID=?;";
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
             "200 OK\n %d %s.balance USD $%.2f\n%s\n",
             quantity, cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}

// Handles the BUY command from the client.
// This command allows a user to buy Pokemon cards from another user (seller).
// It checks balances, updates both users' balances, and updates the card counts.
void handleBuyCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    char cardName[50], cardType[50], rarity[50];
    double price;
    int quantity, buyerID;

    // Expected format: BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>
    // If the input format is wrong, send a 403 format error message.
    if (sscanf(args, "%49s %49s %49s %lf %d %d", cardName, cardType, rarity, &price, &quantity, &buyerID) != 6)
    {
        const char *usageMsg = "403 message format error: Usage -> BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>\n";
        send(clientSocket, usageMsg, strlen(usageMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;

    // Find a seller who has at least one of the requested card
    // Exclude the buyer from being their own seller
    const char *findSellerSQL =
        "SELECT owner_id, count FROM pokemon_cards "
        "WHERE card_name=? AND card_type=? AND rarity=? AND count>0 AND owner_id != ? "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, findSellerSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
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
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        sellerID = sqlite3_column_int(stmt, 0);
        sellerCount = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    // If no seller found, send an error message
    if (sellerID == -1)
    {
        send(clientSocket, "400 invalid command: No seller found for this card.\n",
             strlen("400 invalid command: No seller found for this card.\n"), 0);
        return;
    }

    // Check buyer exists and get buyer's balance
    double buyerBalance = 0.0;
    double totalPrice = 0.0;
    const char *buyerCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, buyerCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error while checking buyer balance.\n",
             strlen("400 invalid command: Database error while checking buyer balance.\n"), 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, buyerID);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        buyerBalance = sqlite3_column_double(stmt, 0);
    }
    else
    {
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: Buyer does not exist.\n",
             strlen("400 invalid command: Buyer does not exist.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Calculate total cost of the transaction
    totalPrice = price * quantity;
    // If buyer cannot afford, cancel transaction
    if (buyerBalance < totalPrice)
    {
        const char *msg = "400 invalid command: Insufficient balance for this purchase.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // Update or delete seller's card record depending on remaining count
    const char *updateSellerSQL;
    if (sellerCount == quantity)
    {
        updateSellerSQL = "DELETE FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }
    else
    {
        updateSellerSQL = "UPDATE pokemon_cards SET count=count-? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }

    if (sqlite3_prepare_v2(db, updateSellerSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error when updating seller.\n",
             strlen("400 invalid command: Database error when updating seller.\n"), 0);
        return;
    }

    // Bind values depending on which SQL query was chosen
    if (sellerCount == quantity)
    {
        sqlite3_bind_int(stmt, 1, sellerID);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, rarity, -1, SQLITE_STATIC);
    }
    else
    {
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_int(stmt, 2, sellerID);
        sqlite3_bind_text(stmt, 3, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, rarity, -1, SQLITE_STATIC);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Check if the buyer already owns the same type of card
    const char *checkBuyerSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
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
    if (buyerCount > 0)
    {
        const char *updateBuyerSQL = "UPDATE pokemon_cards SET count=count+? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
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
    else
    {
        const char *insertBuyerSQL = "INSERT INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES (?, ?, ?, ?, ?);";
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
    const char *updateBuyerBalance = "UPDATE users SET usd_balance=usd_balance-? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBuyerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, buyerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Add total price to seller's balance
    const char *updateSellerBalance = "UPDATE users SET usd_balance=usd_balance+? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateSellerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, sellerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Get updated buyer balance
    double newBuyerBalance = 0.0;
    sqlite3_stmt *balanceStmt;
    const char *getNewBalanceSQL = "SELECT usd_balance FROM users WHERE ID=?;";
    sqlite3_prepare_v2(db, getNewBalanceSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW)
    {
        newBuyerBalance = sqlite3_column_double(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    // Get buyer's new total card count for this card
    int newBuyerCount = 0;
    const char *getBuyerCountSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=?;";
    sqlite3_prepare_v2(db, getBuyerCountSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    sqlite3_bind_text(balanceStmt, 2, cardName, -1, SQLITE_STATIC);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW)
    {
        newBuyerCount = sqlite3_column_int(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    char response[256];
    snprintf(response, sizeof(response),
             "200 OK\nBOUGHT: New balance: %d %s. User USD balance $%.2f\n%s",
             newBuyerCount, cardName, newBuyerBalance, serverPrompt);

    send(clientSocket, response, strlen(response), 0);
}

// Handles the BALANCE command.
// Expected format: BALANCE <userID>
// This command checks a user's USD balance in the database and returns it to the client.
void handleBalanceCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    int userID;

    if (sscanf(args, "%d", &userID) != 1)
    {
        const char *errMsg = "403 message format error\nUsage: BALANCE <OwnerID>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;
    const char *query = "SELECT usd_balance FROM users WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while checking balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    // Bind user ID value into the prepared SQL statement
    sqlite3_bind_int(stmt, 1, userID);

    char response[256];

    // Execute the query and check if a record is found
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double balance = sqlite3_column_double(stmt, 0);

        snprintf(response, sizeof(response), "200 OK\nBalance: %.2f USD\n%s", balance, serverPrompt);
    }
    else
    {
        snprintf(response, sizeof(response),
                 "400 invalid command\n");
    }

    // Finalize the SQL statement and send response
    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}

void handleDepositCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    int userID;
    double amount;

    if (sscanf(args, "%d %lf", &userID, &amount) != 2 || amount <= 0)
    {
        const char *errMsg = "403 message format error\nUsage: DEPOSIT <OwnerID> <amount>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;
    const char *updateSQL = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, updateSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while processing deposit.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_bind_double(stmt, 1, amount);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        const char *errMsg = "400 invalid command\nFailed to update balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);

    // Retrieve updated balance
    const char *querySQL = "SELECT usd_balance FROM users WHERE ID = ?;";
    if (sqlite3_prepare_v2(db, querySQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while retrieving balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_bind_int(stmt, 1, userID);

    char response[256];

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double newBalance = sqlite3_column_double(stmt, 0);
        snprintf(response, sizeof(response), "200 OK\nNew Balance: %.2f USD\n%s", newBalance, serverPrompt);
    }
    else
    {
        snprintf(response, sizeof(response), "400 invalid command\n");
    }

    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}

// Handles the LOOKUP command
//  Expected format: LOOKUP <card_name> || <type> || <rarity>
//  This command checks if there are any cards that matches the card the user entered, if so it returns the ID,
//  Card Name, Type, Rarity, Count, and Owner of the card
void handleLookupCommand(sqlite3 *db, int clientSocket, char *args)
{
    char arg1[50] = "", arg2[50] = "", arg3[50] = "";

    // Trim leading whitespace
    while (*args == ' ' || *args == '\t' || *args == '\n' || *args == '\r')
    {
        args++;
    }

    // Parse up to three arguments
    int count = sscanf(args, "%49s %49s %49s", arg1, arg2, arg3);

    // If no arguments were provided, send format error
    if (count < 1)
    {
        send(clientSocket,
             "403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n",
             strlen("403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n"),
             0);
        return;
    }

    char response[4096];
    memset(response, 0, sizeof(response));

    // Build SQL query depending on how many arguments there are
    const char *baseSQL =
        "SELECT id, card_name, card_type, rarity, count, owner_id "
        "FROM pokemon_cards WHERE "
        "(card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?)";

    if (count >= 2)
    {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }
    if (count == 3)
    {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, baseSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        snprintf(response, sizeof(response), "500 Internal Server Error: %s\n", sqlite3_errmsg(db));
        send(clientSocket, response, strlen(response), 0);
        return;
    }

#define BIND_LIKE(index, value)                                        \
    do                                                                 \
    {                                                                  \
        char pattern[64];                                              \
        snprintf(pattern, sizeof(pattern), "%%%s%%", value);           \
        sqlite3_bind_text(stmt, index, pattern, -1, SQLITE_TRANSIENT); \
    } while (0)

    if (count >= 1)
    {
        BIND_LIKE(1, arg1);
        BIND_LIKE(2, arg1);
        BIND_LIKE(3, arg1);
    }
    if (count >= 2)
    {
        BIND_LIKE(4, arg2);
        BIND_LIKE(5, arg2);
        BIND_LIKE(6, arg2);
    }
    if (count == 3)
    {
        BIND_LIKE(7, arg3);
        BIND_LIKE(8, arg3);
        BIND_LIKE(9, arg3);
    }

    strcat(response, "200 OK\n\n");
    strcat(response, "ID     Card Name           Type       Rarity     Count     Owner\n");
    strcat(response, "--------------------------------------------------------------------\n");

    int rowsFound = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        rowsFound++;

        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *type = sqlite3_column_text(stmt, 2);
        const unsigned char *rarity = sqlite3_column_text(stmt, 3);
        int countValue = sqlite3_column_int(stmt, 4);
        const unsigned char *owner = sqlite3_column_text(stmt, 5);

        char line[256];
        snprintf(line, sizeof(line),
                 "%-6d %-18s %-10s %-10s %-9d %-10s\n",
                 id, name, type, rarity, countValue, owner);

        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }

    sqlite3_finalize(stmt);

    if (rowsFound == 0)
    {
        snprintf(response, sizeof(response), "404 error Your search did not match any records.\n");
    }

    send(clientSocket, response, strlen(response), 0);
}

// Handles the WHO command
// Expected format: WHO
// This command (WHICH CAN ONLY BE USED BY THE ROOT USER) will display a list of the active users and the user's IP address
void handleWhoCommand(sqlite3* db, int clientSocket, char* username)
{
    char response[4096];
    memset(response, 0, sizeof(response));

    (void)db; // Not used in this function

    // Check if the requesting user is root
    int is_root_user = 0;
    for (int i = 0; i < 10; i++)
    {
        if (strlen(active_clients[i].username) == 0)
            continue;

        if (strcasecmp(active_clients[i].username, username) == 0)
        {
            is_root_user = active_clients[i].is_root;
            break;
        }
    }

    // If not root, send 401 Unauthorized
    if (!is_root_user)
    {
        const char* unauth_msg = "401 Unauthorized: Only root user can execute WHO\n";
        send(clientSocket, unauth_msg, strlen(unauth_msg), 0);
        return;
    }

    // Build the list of active users
    snprintf(response, sizeof(response), "200 OK\nThe list of the active users:\n");

    for (int i = 0; i < 10; i++)
    {
        if (strlen(active_clients[i].username) == 0)
            continue; // skip empty slots

        char line[128];
        snprintf(line, sizeof(line), "%s %s\n", active_clients[i].username, active_clients[i].ip);
        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }

    // Send the response to the client
    send(clientSocket, response, strlen(response), 0);
}

// Handles the LIST command.
// Expected format: LIST
// Shows all Pokémon cards owned by the current user (or all if root)
void handleListCommand(sqlite3* db, int clientSocket, const char* username)
{
    sqlite3_stmt* stmt;
    char response[4096];
    memset(response, 0, sizeof(response));

    int is_root_user = 0;

    // Check if current user is root (case-insensitive)
    for (int i = 0; i < 10; i++)
    {
        if (strlen(active_clients[i].username) == 0)
            continue;

        if (strcasecmp(active_clients[i].username, username) == 0)
        {
            is_root_user = active_clients[i].is_root;
            break;
        }
    }

    const char* query;
    if (is_root_user)
    {
        query = "SELECT id, card_name, card_type, rarity, count, owner_id FROM pokemon_cards;";
    }
    else
    {
        query = "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE owner_id = ?;";
    }

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char* msg = "400 invalid command\nDatabase error while listing cards.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    if (!is_root_user)
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    int response_len = 0;

    if (is_root_user)
    {
        response_len = snprintf(response, sizeof(response),
            "200 OK\nThe list of records in the cards database:\n"
            "ID   Card Name        Type        Rarity      Count  OwnerID\n");
    }
    else
    {
        response_len = snprintf(response, sizeof(response),
            "200 OK\nThe list of records in the Pokemon cards table for current user, %s:\n"
            "ID   Card Name        Type        Rarity      Count  OwnerID\n",
            username);
    }

    int found = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int id = sqlite3_column_int(stmt, 0);
        const char* cardName = (const char*)sqlite3_column_text(stmt, 1);
        const char* cardType = (const char*)sqlite3_column_text(stmt, 2);
        const char* rarity = (const char*)sqlite3_column_text(stmt, 3);
        int count = sqlite3_column_int(stmt, 4);
        const char* owner = (const char*)sqlite3_column_text(stmt, 5);

        int written = snprintf(response + response_len, sizeof(response) - response_len,
            "%-4d %-15s %-11s %-11s %-6d %-6s\n",
            id, cardName, cardType, rarity, count, owner);

        if (written < 0 || written >= (int)(sizeof(response) - response_len - 1))
            break;

        response_len += written;
        found = 1;
    }

    sqlite3_finalize(stmt);

    if (!found)
    {
        response_len = snprintf(response, sizeof(response),
            "200 OK\nNo cards found for user %s.\n", username);
    }

    send(clientSocket, response, strlen(response), 0);
}

void *workerThread(void *arg)
{
    const char *loginPrompt = "Enter LOGIN followed by username and password\n";
    const char *serverPrompt = "Available commands: BUY, SELL, BALANCE, DEPOSIT, LIST, QUIT, LOGOUT, WHO, LOOKUP\nEnter message to send to server: ";

    while (1)
    {
        Task t = dequeue(&workQueue);
        int sd = t.client_socket;
        char *buf = t.message;

        buf[strcspn(buf, "\r\n")] = 0;

        int slot = -1;
        for (int i = 0; i < FD_SETSIZE; ++i)
        {
            if (client_sockets[i] == sd)
            {
                slot = i;
                break;
            }
        }
        if (slot == -1)
            continue; // client disconnected

        if (login_status[slot] == 0)
        {

            if (strncmp(buf, "LOGIN", 5) == 0)
            {
                char user[50] = {0}, pass[50] = {0};
                if (strlen(buf) <= 6)
                {
                    send(sd, "Usage: LOGIN <username> <password>\n", 36, 0);
                }
                else
                {
                    int extracted = sscanf(buf + 6, "%49s %49s", user, pass);
                    if (extracted != 2)
                    {
                        send(sd, "Usage: LOGIN <username> <password>\n", 36, 0);
                    }
                    else
                    {
                        pthread_mutex_lock(&db_mutex);
                        pthread_mutex_lock(&active_clients_mutex);
                        int success = handleLoginCommand(db, sd, buf + 6);
                        pthread_mutex_unlock(&active_clients_mutex);
                        pthread_mutex_unlock(&db_mutex);

                        if (success)
                        {
                            login_status[slot] = 1;
                            strncpy(username_by_slot[slot], user, sizeof(username_by_slot[slot]) - 1);
                            username_by_slot[slot][sizeof(username_by_slot[slot]) - 1] = '\0';
                            send(sd, "Login successful!\n", 19, 0);
                            send(sd, serverPrompt, strlen(serverPrompt), 0);
                        }
                    }
                }
            }
            else
            {
                send(sd, "401 Unauthorized: Please LOGIN first.\n", 39, 0);
            }
        }
        else
        {

            if (strncmp(buf, "BALANCE", 7) == 0)
                handleBalanceCommand(db, sd, buf + 8, serverPrompt);
            else if (strncmp(buf, "LIST", 4) == 0)
                handleListCommand(db, sd, buf + 5);
            else if (strncmp(buf, "BUY", 3) == 0)
                handleBuyCommand(db, sd, buf + 4, serverPrompt);
            else if (strncmp(buf, "SELL", 4) == 0)
                handleSellCommand(db, sd, buf + 5, serverPrompt);
            else if (strncmp(buf, "DEPOSIT", 7) == 0)
                handleDepositCommand(db, sd, buf + 8, serverPrompt);
            else if (strncmp(buf, "WHO", 3) == 0)
                handleWhoCommand(db, sd, buf + 4);
            else if (strncmp(buf, "LOOKUP", 6) == 0)
                handleLookupCommand(db, sd, buf + 7);
            else if (strcmp(buf, "LOGOUT") == 0)
            {
                send(sd, "You have been logged out.\n", 26, 0);
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, username_by_slot[slot]) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);
                login_status[slot] = 0;
                username_by_slot[slot][0] = '\0';
                send(sd, loginPrompt, strlen(loginPrompt), 0);
            }
            else if (strcmp(buf, "QUIT") == 0)
            {
                send(sd, "Goodbye!\n", 9, 0);
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, username_by_slot[slot]) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);
                close(sd);
                FD_CLR(sd, &master_set);
                client_sockets[slot] = -1;
                login_status[slot] = 0;
                username_by_slot[slot][0] = '\0';
                conn_count--;
            }
            else if (strcmp(buf, "SHUTDOWN") == 0)
            {
                // Determine requester's username
                char requester[50] = "";
                int slot = -1;
                for (int i = 0; i < FD_SETSIZE; ++i)
                {
                    if (client_sockets[i] == sd)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot != -1 && username_by_slot[slot][0] != '\0')
                {
                    strncpy(requester, username_by_slot[slot], sizeof(requester) - 1);
                    requester[sizeof(requester) - 1] = '\0';
                }

                // Check if requester is root
                int is_root_requester = 0;
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, requester) == 0)
                    {
                        if (active_clients[k].is_root)
                            is_root_requester = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);

                const char *discMsg = "200 OK\nServer is disconnecting all clients\n";
                if (is_root_requester)
                    send(sd, "200 OK\nServer shutting down\n", 27, 0);
                else
                    send(sd, discMsg, strlen(discMsg), 0);

                pthread_mutex_lock(&active_clients_mutex);
                for (int j = 0; j < FD_SETSIZE; ++j)
                {
                    if (client_sockets[j] != -1)
                    {
                        if (client_sockets[j] != sd)
                            send(client_sockets[j], "Server: you have been disconnected by SHUTDOWN command.\n", 56, 0);

                        close(client_sockets[j]);
                        client_sockets[j] = -1;
                        login_status[j] = 0;
                        username_by_slot[j][0] = '\0';
                        conn_count--;
                    }
                }
                active_count = 0;
                pthread_mutex_unlock(&active_clients_mutex);

                // Root requester terminates server
                if (is_root_requester)
                {
                    close(serverSocket);
                    sqlite3_close(db);
                    printf("Server terminated by root shutdown command.\n");
                    exit(0);
                }
                else
                {
                    // Non-root requester just disconnects themselves
                    close(sd);
                    if (slot != -1)
                    {
                        client_sockets[slot] = -1;
                        login_status[slot] = 0;
                        username_by_slot[slot][0] = '\0';
                    }
                }
            }
            else
            {
                send(sd, "Invalid command\n", 16, 0);
            }

            // Send server prompt if logged in
            if (client_sockets[slot] != -1 && login_status[slot] == 1)
                send(sd, serverPrompt, strlen(serverPrompt), 0);
        }
    }
    return NULL;
}

void startWorkerPool()
{
#ifdef _WIN32
    HANDLE threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        threads[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)workerThread, NULL, 0, NULL);

        if (threads[i] == NULL)
        {
            fprintf(stderr, "Failed to create thread %d\n", i);
        }
        else
        {
            CloseHandle(threads[i]);
        }
    }
#else
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        pthread_create(&threads[i], NULL, workerThread, NULL);
        pthread_detach(threads[i]);
    }
#endif
}


int main()
{
#ifdef _WIN32
    // Initialize Winsock on Windows for network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // --- Open or create the SQLite database ---
    char* zErrMsg = NULL;
    int rc = sqlite3_open("users.db", &db);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Enable foreign keys (SQLite ignores them unless explicitly turned on)
    rc = sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        const char* msg = zErrMsg ? zErrMsg : sqlite3_errmsg(db);
        fprintf(stderr, "SQL error (PRAGMA foreign_keys): %s\n", msg);
        if (zErrMsg) sqlite3_free(zErrMsg);
        zErrMsg = NULL;
    }

    // Initialize mutexes
#ifdef _WIN32
    db_mutex = CreateMutex(NULL, FALSE, NULL);
    active_clients_mutex = CreateMutex(NULL, FALSE, NULL);
#else
    pthread_mutex_init(&db_mutex, NULL);
    pthread_mutex_init(&active_clients_mutex, NULL);
#endif

    fprintf(stderr, "Opened database successfully\n");

    // --- Create "users" table ---
    zErrMsg = NULL;
    const char* sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "first_name TEXT,"
        "last_name TEXT,"
        "user_name TEXT NOT NULL UNIQUE,"
        "password TEXT,"
        "usd_balance REAL NOT NULL DEFAULT 0,"
        "is_root INTEGER NOT NULL DEFAULT 0"
        ");";

    rc = sqlite3_exec(db, sql_users, NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (create users table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        zErrMsg = NULL;
    }
    else
    {
        printf("Users table created or already exists.\n");
    }

    // --- Insert users ---
    const char* sql_insert_users =
        "INSERT OR IGNORE INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES "
        "(1, 'Branch', 'Tree', 'Root', 'Root01', 100, 1),"
        "(2, 'Ms', 'Partner', 'Mary', 'Mary01', 50, 0),"
        "(3, 'Mr', 'Dude', 'John', 'John01', 200, 0),"
        "(4, 'Ms', 'Misses', 'Moe', 'Moe01', 300, 0);";

    rc = sqlite3_exec(db, sql_insert_users, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (insert users): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        zErrMsg = NULL;
    }
    else
    {
        printf("Default users inserted or already exist.\n");
    }

    // --- Create "pokemon_cards" table ---
    const char* sql_pokemon_cards =
        "CREATE TABLE IF NOT EXISTS pokemon_cards ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "card_name TEXT NOT NULL,"
        "card_type TEXT NOT NULL,"
        "rarity TEXT NOT NULL,"
        "count INTEGER NOT NULL,"
        "owner_id INTEGER NOT NULL,"
        "FOREIGN KEY(owner_id) REFERENCES users(ID),"
        "UNIQUE(card_name, card_type, rarity, owner_id)"
        ");";

    rc = sqlite3_exec(db, sql_pokemon_cards, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (create pokemon_cards table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        zErrMsg = NULL;
    }
    else
    {
        printf("Pokemon cards table created or already exists.\n");
    }

    // --- Insert default Pokémon cards ---
    const char* sql_insert_pokemon =
        "INSERT OR IGNORE INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES "
        "('Pikachu', 'Electric', 'Common', 2, 1),"
        "('Charizard', 'Fire', 'Rare', 3, 2),"
        "('Squirtle', 'Water', 'Uncommon', 30, 2);";

    rc = sqlite3_exec(db, sql_insert_pokemon, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (insert pokemon): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        zErrMsg = NULL;
    }
    else
    {
        printf("Default Pokemon inserted or already exist.\n");
    }


    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9001);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
    {
        perror("bind");
        exit(1);
    }

    if (listen(serverSocket, 10) < 0)
    {
        perror("listen");
        exit(1);
    }

    printf("Server listening on port 9001...\n");

    fd_set master_set, read_fds;
    for (int i = 0; i < FD_SETSIZE; ++i)
    {
        client_sockets[i] = -1;
        login_status[i] = 0;
        username_by_slot[i][0] = '\0';
    }

    FD_ZERO(&master_set);
    FD_SET(serverSocket, &master_set);
    int max_fd = serverSocket;

    // Initialize and start worker threads
    initQueue(&workQueue);
    startWorkerPool();

    const char *loginPrompt = "Enter LOGIN followed by username and password\n";

    while (1)
    {
        read_fds = master_set; // copy for select
        int nready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (nready < 0)
        {
            perror("select");
            break;
        }

        // Handle new connections
        if (FD_ISSET(serverSocket, &read_fds))
        {
            struct sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            int newsock = accept(serverSocket, (struct sockaddr *)&clientAddr, &addrLen);
            if (newsock < 0)
            {
                perror("accept");
                continue;
            }

            if (conn_count >= 10)
            {
                const char *msg = "503 Service Unavailable: Server is full. Please try again later.\n";
                send(newsock, msg, strlen(msg), 0);
                close(newsock);
                printf("Rejected connection; server full.\n");
            }
            else
            {
                int slot = -1;
                for (int i = 0; i < FD_SETSIZE; ++i)
                {
                    if (client_sockets[i] == -1)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot == -1)
                {
                    close(newsock);
                }
                else
                {
                    client_sockets[slot] = newsock;
                    login_status[slot] = 0;
                    username_by_slot[slot][0] = '\0';
                    FD_SET(newsock, &master_set);
                    if (newsock > max_fd)
                        max_fd = newsock;
                    conn_count++;
                    send(newsock, loginPrompt, strlen(loginPrompt), 0);
                    printf("Accepted connection on socket %d (slot %d)\n", newsock, slot);
                }
            }
            if (--nready <= 0)
                continue;
        }

        // Handle data from clients
        for (int i = 0; i < FD_SETSIZE && nready > 0; ++i)
        {
            int sd = client_sockets[i];
            if (sd == -1)
                continue;
            if (!FD_ISSET(sd, &read_fds))
                continue;

            nready--;
            char buf[512];
            memset(buf, 0, sizeof(buf));
            int bytes = recv(sd, buf, sizeof(buf) - 1, 0);
            if (bytes <= 0)
            {
                // client disconnected
                printf("Client on socket %d disconnected\n", sd);
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, username_by_slot[i]) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);

                close(sd);
                FD_CLR(sd, &master_set);
                client_sockets[i] = -1;
                login_status[i] = 0;
                username_by_slot[i][0] = '\0';
                conn_count--;
                continue;
            }

            // Enqueue task for worker thread
            Task t = {.client_socket = sd};
            strncpy(t.message, buf, sizeof(t.message) - 1);
            t.message[sizeof(t.message) - 1] = '\0';
            enqueue(&workQueue, t);
        }
    }

    // cleanup
    close(serverSocket);
    sqlite3_close(db);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}