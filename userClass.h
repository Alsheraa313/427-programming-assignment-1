#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>


static int callback(void *NotUsed, int argc, char **argv, char **azColName) {
   int i;
   for(i = 0; i<argc; i++) {
      printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
   }
   printf("\n");
   return 0;
}

int main() {
    sqlite3 *db;
    char *zErrMsg = 0;
    char *sql;
    int rc;
    const char* data = "Callback called";

    /* Open database */
    rc = sqlite3_open("users", &db);

    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    fprintf(stderr, "Opened database successfully\n");

    sql =
"CREATE TABLE IF NOT EXISTS users ("
"ID INTEGER PRIMARY KEY,"
"first_name TEXT,"
"last_name TEXT,"
"user_name TEXT NOT NULL,"
"password TEXT,"
"usd_balance DOUBLE NOT NULL,"
"is_root INTEGER NOT NULL DEFAULT 0)";

    rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else{
        fprintf(stdout, "Table created successfully\n");
    }

 
sql =   "INSERT INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES"
        "(1, 'chewa', 'shewa', 'shewashewa', 'passwerd', 100, 1),"
        "(2, 'mr', 'partner', 'mr.partner', 'pass', 50, 0)";
        rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
        if(rc != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", zErrMsg);
            sqlite3_free(zErrMsg);
        } else {
            fprintf(stdout, "Records created successfully\n");
        }
    

   sql = "SELECT * from users";
   rc = sqlite3_exec(db, sql, callback, (void*)data, &zErrMsg);

   if( rc != SQLITE_OK ){
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
   } else {
      fprintf(stdout, "Operation done successfully\n");
   }

   /* Create merged SQL statement */
   sql = "UPDATE users set count = 1 where ID=1; " \
         "SELECT * from users";


    rc = sqlite3_exec(db, sql, callback, (void*)data, &zErrMsg);
    
    if( rc != SQLITE_OK ) {
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
   } else {
      fprintf(stdout, "Operation done successfully\n");
   }
   sqlite3_close(db);
    return 0;
}
