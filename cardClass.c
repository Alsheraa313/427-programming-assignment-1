#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pokemonCard {
    int cardPrice;
    char cardname[30];
    int quantity;


    void (*setPrice)(struct pokemonCard *, int);
    int (*getPrice)(struct pokemonCard *);

    void (*setQuantity)(struct pokemonCard *, int);
    int (*getQuantity)(struct pokemonCard *);

    void (*setName)(struct pokemonCard *, const char *);
    const char *(*getName)(struct pokemonCard *);
};


void setPrice(struct pokemonCard *inst, int cardPrice) {
    inst->cardPrice = cardPrice;
}

int getPrice(struct pokemonCard *inst) {
    return inst->cardPrice;
}


void setQuantity(struct pokemonCard *inst, int quantity) {
    inst->quantity = quantity;
}

int getQuantity(struct pokemonCard *inst) {
    return inst->quantity;
}


void setName(struct pokemonCard *inst, const char *name) {
    strncpy(inst->cardname, name, sizeof(inst->cardname) - 1);
    inst->cardname[sizeof(inst->cardname) - 1] = '\0';
}

const char *getName(struct pokemonCard *inst) {
    return inst->cardname;
}

int main() {
    struct pokemonCard pikachu;

    pikachu.setPrice = setPrice;
    pikachu.getPrice = getPrice;

    pikachu.setQuantity = setQuantity;
    pikachu.getQuantity = getQuantity;

    pikachu.setName = setName;
    pikachu.getName = getName;

    pikachu.setPrice(&pikachu, 10);
    pikachu.setQuantity(&pikachu, 3);
    pikachu.setName(&pikachu, "pikachu");


    printf("pokemon card: %s\n", pikachu.getName(&pikachu));
    printf("price: $%d\n", pikachu.getPrice(&pikachu));
    printf("quantity: %d\n", pikachu.getQuantity(&pikachu));

    return 0;
}
