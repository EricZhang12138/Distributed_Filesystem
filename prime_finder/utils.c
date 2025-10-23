#include <stdio.h>
#include <stdlib.h>
#include "utils.h"

// Hash function to get the index of the key in the hash set
unsigned int hash(uint64_t key, int size){
    return key % size;
}

// Creates a new hash set with the given size
HashSet* create_hash_set(int size){
    HashSet* hs = (HashSet*)malloc(sizeof(HashSet));
    hs->size = size;
    hs->table = (Node**)calloc(size, sizeof(Node*));
    return hs;
}

int hash_set_insert(HashSet* hs, uint64_t key){
    unsigned int index = hash(key, hs->size);
    Node* current = hs->table[index];
    while (current != NULL) {
        if (current->key == key) {
            return 0;
        }
        current = current->next;
    }
    
    Node* newNode = (Node*)malloc(sizeof(Node));
    newNode->key = key;
    newNode->next = hs->table[index];
    hs->table[index] = newNode;
    
    return 1;
}

void free_hash_set(HashSet* hs){
    for (int i = 0; i < hs->size; ++i){
        Node* current = hs->table[i];
        while (current != NULL) {
            Node* temp = current;
            current = current->next;
            free(temp);
        }
    }
    free(hs->table);
    free(hs);
}
