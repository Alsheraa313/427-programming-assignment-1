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
    rc = sqlite3_open("cards", &db);

    if(rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    fprintf(stderr, "Opened database successfully\n");

    sql =
"CREATE TABLE IF NOT EXISTS Cards ("
"ID INTEGER PRIMARY KEY,"
"card_name TEXT NOT NULL,"
"card_type TEXT NOT NULL,"
"rarity TEXT NOT NULL,"
"price INTEGER,"
"count INTEGER,"
"owner_id INTEGER,"
"FOREIGN KEY (owner_id) REFERENCES Users(ID))"
;

    rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (create): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else{
        fprintf(stdout, "Table created successfully\n");
    }

 
sql =   "INSERT INTO Cards (ID, card_name, card_type, rarity, price, count, owner_id) VALUES"
        "(1, 'pikachu', 'electric', 'rare', 100, 2, 1),"
        "(2, 'charmander', 'fire', 'common', 50, 5, 1),"
        "(3, 'dragonite', 'dragon', 'legendary', 1000, 1, 2)";
        rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
        if(rc != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", zErrMsg);
            sqlite3_free(zErrMsg);
        } else {
            fprintf(stdout, "Records created successfully\n");
        }
    

   sql = "SELECT * from Cards";
   rc = sqlite3_exec(db, sql, callback, (void*)data, &zErrMsg);

   if( rc != SQLITE_OK ){
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
   } else {
      fprintf(stdout, "Operation done successfully\n");
   }

   /* Create merged SQL statement */
   sql = "UPDATE Cards set count = 1 where ID=1; " \
         "SELECT * from Cards";


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
