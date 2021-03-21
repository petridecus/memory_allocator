
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>

#include "hmalloc.h"

const size_t PAGE_SIZE = 4096;
static hm_stats stats;
static node* head = NULL;
// static uintptr_t loc = 0xabcd0000;

long
free_list_length() {
    node* nn = head;
    long len = 0;

    while (nn) {
        len++;
        nn = nn->next;
    }

    return len;
}

hm_stats*
hgetstats() {
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats() {
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

static
size_t
div_up(size_t xx, size_t yy) {
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

 /**
  * Handles coalescing after a new block has been inserted.
  * ------------------------------------------------------------------
  * Cases:
  * - Forwards:
  *     If the new node lines up with ONLY the next one, then we merge
  *     next into new.
  *
  * - Backwards:
  *     If the prev node lines up with the new one, then we merge
  *     new into prev.
  *
  * - Merging all 3:
  *     Case for coalescing all 3 is covered within backwards block,
  *     which *immediately* returns in that case.
  * ------------------------------------------------------------------
  * Params:
  *  - prev: The node that is before the new node in the free list.
  *
  *  - new: A node that was just added to the free list.
  *
  *  - next: The node that is after the new node in the free list.
  */
void
coalesce(node* prev, node* new, node* next) {
    if ((!prev
         || (uintptr_t)prev + prev->size != (uintptr_t)new)
        && next
        && (uintptr_t)new + new->size == (uintptr_t)next) {

        // case where we only coalesce forwards.
        new->size += next->size;
        new->next = next->next;
    } else if ((!next
                || (uintptr_t)new + new->size != (uintptr_t)next)
               && prev
               && (uintptr_t)prev + prev->size == (uintptr_t)new) {

        // case where we only coalesce backwards.
        prev->size += new->size;
        prev->next = next;
    } else if (next && prev
               && (uintptr_t)prev + prev->size == (uintptr_t) new
               && (uintptr_t)new + new->size == (uintptr_t)next) {

        // case where all 3 coalesce
        prev->size += new->size;
        prev->size += next->size;
        prev->next = next->next;
    }
}

/**
 * Adds a new memory block to the free list.
 * --------------------------------------------------------------------
 * Cases:
 *
 *  - If the free list is empty, it initializes the head at
 *    the given location.
 *
 *  - Otherwise, we search for the first node with a memory location
 *    greater than the given location, and if one is found we place
 *    a node at the given location betwen the previous node
 *    and the current one.
 *
 *  - If we exit the loop without finding a greater node, we insert
 *    the new node at the end.
 * --------------------------------------------------------------------
 * In both the second and third cases, the coalesce
 * function is called to combine adjacent memory
 * addresses after insertion.
 * --------------------------------------------------------------------
 * Params:
 *
 *  - location: A pointer to the location in memory where the free
 *              block to be added to the free list exists.
 *
 *  - size: The size of the free block being added to the free list.
 */
void
free_block_insert(void* location, size_t size) {
    if (!head) {
        head = location;
        head->size = size;
        head->next = NULL;
        return;
    }

    node* nn = head;
    node* last = NULL;
    node* new = location;
    new->size = size;

    while (nn) {
        if (location < (void*)nn) {
            new->next = nn;
            if (!last) {
                head = new;
            } else {
                last->next = new;
            }
            coalesce(last, new, nn);
            return;
        } else {
            last = nn;
            nn = nn->next;
        }
    }

    last->next = new;
    new->next = nn;
    coalesce(last, new, NULL);
}

/**
 * Updates the free block at the given location.
 * -------------------------------------------------------------------
 * Iterates through the current free list.
 *
 * As soon as a node is found that corresponds to the given location,
 * its location is pushed forwards by the given size, and its size
 * is shrunk accordingly.
 * -------------------------------------------------------------------
 * Params:
 *
 *  - old_loc: The starting point of the node to being adjusted.
 *             *Must* point to a node in the free list in memory.
 *
 *  - size: The amount by which the starting point of the changing
 *          block is being pushed forward (only the starting point,
 *          not the whole block).
 *
 * -------------------------------------------------------------------
 * Throws if there isn't a free block node at the given location.
 */
void
free_block_update(void* old_loc, size_t size) {
    node* nn = head;
    node* prev = NULL;

    while (nn) {
        if ((uintptr_t)nn == (uintptr_t)old_loc) {
            // populate an updated node with the contents of nn,
            // and then subtract the given size
            node* updated = (node*)((uintptr_t)old_loc + size);
            if (nn->next) updated->next = nn->next;
            updated->size = nn->size;
            updated->size -= size;

            // conditional to handle cases with/without previous node
            // and when the new node can just get scrapped (size 0).
            if (prev && updated->size) {
                prev->next = updated;
            } else if (prev) {
                prev->next = updated->next;
            } else if (updated->size){
                head = updated;
            } else {
                head = nn->next;
            }

            return;
        } else {
            prev = nn;
            nn = nn->next;
        }
    }

    perror("calling block update on a block that doesn't exist!");
    exit(EXIT_FAILURE);
}

/**
 * Handles requests to allocate memory on the heap.
 * -------------------------------------------------------------------
 * Steps:
 *
 *  - Checks if there is a block large enough to accomodate the
 *    requested size. If there is, that block is updated within free
 *    memory and we return its old location.
 *
 *  - If none of the blocks on the free list are sufficient, a new
 *    block of memory with a number of pages sufficient to accommodate
 *    the requested size is mapped.
 *
 *  - If the requested size fits within 1 block we insert the
 *    remainder of the block onto the free list.
 *
 *  - If the requested size required a multi-page block, nothing is
 *    added to the free list.
 *
 * -------------------------------------------------------------------
 * Params:
 *
 *  - size: The size of the block to be allocated on the heap.
 *
 * -------------------------------------------------------------------
 * Returns:
 *
 * A pointer to the location of the memory that has been allocated.
 */
void*
hmalloc(size_t size) {
    stats.chunks_allocated += 1;

    node* nn = head;
    size += sizeof(size_t);

    while (nn) {
        if (size <= nn->size) {
            free_block_update((void*)nn, size);
            *(size_t*)nn = size;
            return (void*)nn + sizeof(size_t);
        } else {
            nn = nn->next;
        }
    }

    long num_pages = div_up(size, PAGE_SIZE);
    size_t ss = num_pages * PAGE_SIZE;

    stats.pages_mapped += num_pages;

    void* loc = mmap(NULL, ss,
                    PROT_READ|PROT_WRITE,
                    MAP_ANON|MAP_PRIVATE,
                    -1, 0);

    if (loc == MAP_FAILED) {
        perror("the map failed");
        exit(EXIT_FAILURE);
    }

    *(size_t*)loc = size;

    if (num_pages == 1) free_block_insert(loc + size, ss - size);
    return loc + sizeof(size_t);
}

/**
 * Frees the used memory block represented by the given pointer.
 * -------------------------------------------------------------------
 * Steps:
 *  - Determines the size of the given item by tracing back by 8 bytes
 *    to the size metadata that was stored during hmalloc.
 *
 *  - If the found size fits in 1 page, we just return the whole given
 *    block to the free list.
 *
 *  - If the found size takes up multiple pages, we unmap
 *    the entire thing.
 *
 * -------------------------------------------------------------------
 * Params:
 *  - item: Points to a location in memory that was previously
 *          allocated by hmalloc. Within this scope, we know that
 *          the previous 8 bytes contain information on the size
 *          of the block that "item" points to.
 */
void
hfree(void* item) {
    stats.free_length = free_list_length();
    stats.chunks_freed += 1;

    item = item - sizeof(size_t);
    size_t ss = *(size_t*)(uintptr_t)item;
    long num_pages = div_up(ss, PAGE_SIZE);

    if (num_pages > 1) {
        stats.pages_unmapped += num_pages;
        munmap(item, ss);
    } else {
        free_block_insert(item, ss);
    }
}
