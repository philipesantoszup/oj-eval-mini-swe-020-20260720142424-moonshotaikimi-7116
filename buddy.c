#include "buddy.h"
#include <stdio.h>
#include <stdint.h>

#define NULL ((void *)0)
#define MAX_RANK 16

typedef struct Block {
    void *start;
    struct Block *next;
} Block;

#define META_MAGIC 0xDEADBEEF

typedef struct {
    int rank;
    int magic;
} AllocMeta;

static void *pool_start = NULL;
static int pool_pgcount = 0;

static Block *free_list[MAX_RANK + 1];

static int rank_pages(int rank) {
    return 1 << (rank - 1);
}

static long long rank_size_bytes(int rank) {
    return (long long)rank_pages(rank) * 4096LL;
}

static int is_in_pool(void *p) {
    if (!p) return 0;
    long long addr = (long long)p;
    long long start = (long long)pool_start;
    long long end = start + (long long)pool_pgcount * 4096LL;
    return addr >= start && addr < end;
}

static int is_in_free_list(int rank, void *p) {
    Block *curr = free_list[rank];
    while (curr) {
        if (curr->start == p) return 1;
        curr = curr->next;
    }
    return 0;
}

static void remove_from_free_list(int rank, void *p) {
    Block **curr = &free_list[rank];
    while (*curr) {
        if ((*curr)->start == p) {
            *curr = (*curr)->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

// Get buddy address using XOR
static void *buddy_addr(void *p, int rank) {
    long long size = rank_size_bytes(rank);
    return (void *)(((long long)p - (long long)pool_start) ^ size) + (long long)pool_start;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) return -EINVAL;
    
    pool_start = p;
    pool_pgcount = pgcount;
    
    for (int i = 1; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }
    
    void *curr = p;
    int remaining = pgcount;
    
    while (remaining > 0) {
        int rank = MAX_RANK;
        int pages = rank_pages(rank);
        while (rank > 1 && pages > remaining) {
            rank--;
            pages = rank_pages(rank);
        }
        
        if (rank < 1 || pages > remaining) break;
        
        Block *block = curr;
        block->start = curr;
        block->next = free_list[rank];
        free_list[rank] = block;
        
        curr += pages * 4096LL;
        remaining -= pages;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }
    
    int found_rank = rank;
    while (found_rank <= MAX_RANK && free_list[found_rank] == NULL) {
        found_rank++;
    }
    
    if (found_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }
    
    Block *block = free_list[found_rank];
    free_list[found_rank] = block->next;
    void *result = block->start;
    
    // Split down to requested rank
    while (found_rank > rank) {
        found_rank--;
        long long split_size = rank_size_bytes(found_rank);
        void *buddy = result + split_size;
        
        Block *buddy_block = buddy;
        buddy_block->start = buddy;
        buddy_block->next = free_list[found_rank];
        free_list[found_rank] = buddy_block;
    }
    
    AllocMeta *meta = (AllocMeta *)result;
    meta->rank = rank;
    meta->magic = META_MAGIC;
    
    return result;
}

int return_pages(void *p) {
    if (!p) return -EINVAL;
    if (!is_in_pool(p)) return -EINVAL;
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic != META_MAGIC) return -EINVAL;
    
    int rank = meta->rank;
    meta->magic = 0;
    
    void *addr = p;
    
    // Try to coalesce
    while (rank < MAX_RANK) {
        void *buddy = buddy_addr(addr, rank);
        
        // Check if buddy is a valid free block at this rank
        if (!is_in_pool(buddy)) break;
        if (!is_in_free_list(rank, buddy)) break;
        
        // Merge: remove buddy from free list
        remove_from_free_list(rank, buddy);
        
        // Combined block starts at lower address
        if (buddy < addr) {
            addr = buddy;
        }
        rank++;
    }
    
    // Add to free list
    Block *block = addr;
    block->start = addr;
    block->next = free_list[rank];
    free_list[rank] = block;
    
    return OK;
}

int query_ranks(void *p) {
    if (!p) return -EINVAL;
    if (!is_in_pool(p)) return -EINVAL;
    
    // Check free lists
    for (int r = MAX_RANK; r >= 1; r--) {
        Block *curr = free_list[r];
        while (curr) {
            if (curr->start == p) return r;
            curr = curr->next;
        }
    }
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic == META_MAGIC) {
        return meta->rank;
    }
    
    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }
    
    int count = 0;
    Block *curr = free_list[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }
    
    return count;
}
