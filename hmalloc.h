#ifndef HMALLOC_H
#define HMALLOC_H

// Husky Malloc Interface
// cs3650 Starter Code
#include<stdlib.h>

typedef struct hm_stats {
    long pages_mapped;
    long pages_unmapped;
    long chunks_allocated;
    long chunks_freed;
    long free_length;
} hm_stats;

// does there need to be a separate free_list
typedef struct node_t {
    size_t size;
    struct node_t* next;
} node;

//typedef struct free_block_t {
//    long size;
//    struct free_block_t* next;
//} free_block;

hm_stats* hgetstats();
void hprintstats();

void* hmalloc(size_t size);
void hfree(void* item);

#endif
