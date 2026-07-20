#include "buddy.h"
#include <stdio.h>
#include <stdint.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define MAX_PAGES 262144  // 2^18, reasonable upper bound for this problem

// Block structure for free list
typedef struct Block {
    void *start;
    struct Block *next;
} Block;

#define META_MAGIC 0xDEADBEEF

typedef struct {
    int rank;
    int magic;
} AllocMeta;

// Track state of each page
// Bit 0: allocated (1) or free (0)
// Bits 4-7: rank of block (for free blocks, this is the rank of the containing free block)
static unsigned char page_state[MAX_PAGES];
static int page_rank[MAX_PAGES];  // Current rank of the block starting at this page

// Global state
static void *pool_start = NULL;
static int pool_pgcount = 0;
static int max_pages = 0;

// Free lists
static Block *free_list[MAX_RANK + 1];

// Pool address to page index
static inline int addr_to_page(void *p) {
    return (int)(((long long)p - (long long)pool_start) >> 12);
}

// Helper: get pages for a rank
static inline int rank_pages(int rank) {
    return 1 << (rank - 1);
}

// Helper: check if address is in pool
static inline int is_in_pool(void *p) {
    int idx = addr_to_page(p);
    return idx >= 0 && idx < max_pages;
}

// Initialize
int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) return -EINVAL;
    
    pool_start = p;
    pool_pgcount = pgcount;
    max_pages = pgcount;
    
    // Clear state
    for (int i = 0; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }
    for (int i = 0; i < max_pages; i++) {
        page_state[i] = 0;
        page_rank[i] = 0;
    }
    
    // Build free blocks from largest to smallest
    int pos = 0;
    while (pos < pgcount) {
        int rank = MAX_RANK;
        int pages = rank_pages(rank);
        while (rank > 1 && pos + pages > pgcount) {
            rank--;
            pages = rank_pages(rank);
        }
        
        if (pos + pages > pgcount) break;
        
        // Add to free list
        Block *block = (Block *)((char *)p + ((long long)pos << 12));
        block->start = block;
        block->next = free_list[rank];
        free_list[rank] = block;
        
        // Mark pages as free at this rank
        for (int i = pos; i < pos + pages && i < max_pages; i++) {
            page_state[i] = 0;
            page_rank[i] = rank;
        }
        
        pos += pages;
    }
    
    return OK;
}

// Allocate pages
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
    
    // Pop from free list
    Block *block = free_list[found_rank];
    free_list[found_rank] = block->next;
    void *result = block;
    
    // Split down to requested rank
    int start_page = addr_to_page(result);
    while (found_rank > rank) {
        found_rank--;
        int split_pages = rank_pages(found_rank);
        void *buddy = (char *)result + ((long long)split_pages << 12);
        
        // Push buddy to free list
        Block *buddy_block = (Block *)buddy;
        buddy_block->start = buddy;
        buddy_block->next = free_list[found_rank];
        free_list[found_rank] = buddy_block;
        
        // Mark buddy pages as free at this rank
        int buddy_page = start_page + split_pages;
        for (int i = buddy_page; i < buddy_page + split_pages; i++) {
            page_state[i] = 0;
            page_rank[i] = found_rank;
        }
        
        start_page = buddy_page - split_pages; // Keep track of result start
    }
    
    // Mark result pages as allocated
    int res_page = addr_to_page(result);
    for (int i = res_page; i < res_page + rank_pages(rank); i++) {
        page_state[i] = 1;
        page_rank[i] = rank;
    }
    
    // Store metadata
    AllocMeta *meta = (AllocMeta *)result;
    meta->rank = rank;
    meta->magic = META_MAGIC;
    
    return result;
}

// Get buddy address
static inline void *buddy_addr(void *p, int rank) {
    long long offset = (long long)p - (long long)pool_start;
    long long size = (long long)rank_pages(rank) << 12;
    return (void *)((offset ^ size) + (long long)pool_start);
}

// Return pages
int return_pages(void *p) {
    if (!p) return -EINVAL;
    if (!is_in_pool(p)) return -EINVAL;
    
    // Verify it's allocated
    int pg = addr_to_page(p);
    if (!page_state[pg] || page_rank[pg] == 0) return -EINVAL;
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic != META_MAGIC) return -EINVAL;
    
    int rank = meta->rank;
    meta->magic = 0;
    
    void *addr = p;
    int start_pg = addr_to_page(addr);
    
    // Mark as free in our tracking
    for (int i = start_pg; i < start_pg + rank_pages(rank); i++) {
        page_state[i] = 0;
    }
    
    // Coalesce
    while (rank < MAX_RANK) {
        void *buddy = buddy_addr(addr, rank);
        int buddy_pg = addr_to_page(buddy);
        
        // Check if buddy is valid and free at this rank
        if (buddy_pg < 0 || buddy_pg >= max_pages) break;
        if (page_state[buddy_pg] != 0) break;
        if (page_rank[buddy_pg] != rank) break;
        
        // Remove buddy from free list - O(1) using page_addr lookup
        Block *buddy_block = (Block *)buddy;
        Block **curr = &free_list[rank];
        while (*curr) {
            if (*curr == buddy_block) {
                *curr = (*curr)->next;
                break;
            }
            curr = &(*curr)->next;
        }
        
        // Update address to lower of the two
        if (buddy < addr) {
            addr = buddy;
            start_pg = buddy_pg;
        }
        rank++;
    }
    
    // Add to free list
    Block *block = (Block *)addr;
    block->start = addr;
    block->next = free_list[rank];
    free_list[rank] = block;
    
    // Update page rank for the merged block
    for (int i = start_pg; i < start_pg + rank_pages(rank); i++) {
        page_rank[i] = rank;
    }
    
    return OK;
}

// Query rank
int query_ranks(void *p) {
    if (!p) return -EINVAL;
    if (!is_in_pool(p)) return -EINVAL;
    
    int pg = addr_to_page(p);
    if (pg < 0 || pg >= max_pages) return -EINVAL;
    
    // Check if allocated
    if (page_state[pg]) {
        AllocMeta *meta = (AllocMeta *)p;
        if (meta->magic == META_MAGIC) {
            return meta->rank;
        }
        return -EINVAL;
    }
    
    // Free block - return its rank
    return page_rank[pg];
}

// Query free page count
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
