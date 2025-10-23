#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

// A simple hashset implementation to store unique prime numbers

typedef struct Node {
    uint64_t key;
    struct Node* next;
} Node;

typedef struct HashSet {
    int size;
    Node** table;
} HashSet;

// Creates a new hashset
HashSet* create_hash_set(int size);

// Inserts a key into the hashset
// Returns 1 if the key was added, 0 if it already existed
int hash_set_insert(HashSet* hs, uint64_t key);

// Frees the memory used by the hashset
void free_hash_set(HashSet* hs);

#endif
