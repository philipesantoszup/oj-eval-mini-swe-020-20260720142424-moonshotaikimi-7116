#include "buddy.h"

#define NULL ((void *)0)
#define MAX_RANK 16
#define MAX_PAGES 262144

typedef struct Block {
    void *start;
    struct Block *next;
    struct Block *prev;  // For O(1) removal
} Block;

#define META_MAGIC 0xDEADBEEF

typedef struct {
    int rank;
    int magic;
} AllocMeta;

static void *pool_start = NULL;
static int pool_pgcount = 0;
static int max_pages = 0;

// Use static arrays for O(1) lookup
// For each page, store: 0 = allocated, rank (1-16) = free at that rank
static unsigned char page_rank[MAX_PAGES];  // 0 = allocated, otherwise rank of free block
static Block *page_block[MAX_PAGES];  // Pointer to Block struct for free pages

// Free list heads
static Block *free_list[MAX_RANK + 1];

// Convert address to page index
static inline int addr_to_page(void *p) {
    return (int)(((long long)p - (long long)pool_start) >> 12);
}

// Get pages for a rank
static inline int rank_pages(int rank) {
    return 1 << (rank - 1);
}

// Check if in pool
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
    
    for (int i = 0; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }
    for (int i = 0; i < max_pages; i++) {
        page_rank[i] = 0;
        page_block[i] = NULL;
    }
    
    int pos = 0;
    while (pos < pgcount) {
        int rank = MAX_RANK;
        int pages = rank_pages(rank);
        while (rank > 1 && pos + pages > pgcount) {
            rank--;
            pages = rank_pages(rank);
        }
        
        if (pos + pages > pgcount) break;
        
        Block *block = (Block *)((char *)p + ((long long)pos << 12));
        block->start = block;
        block->next = free_list[rank];
        block->prev = NULL;
        if (free_list[rank]) {
            free_list[rank]->prev = block;
        }
        free_list[rank] = block;
        
        for (int i = pos; i < pos + pages && i < max_pages; i++) {
            page_rank[i] = rank;
            page_block[i] = block;
        }
        
        pos += pages;
    }
    
    return 0;
}

// Calculate buddy address
static inline void *buddy_addr(void *p, int rank) {
    long long offset = (long long)p - (long long)pool_start;
    long long size = (long long)rank_pages(rank) << 12;
    return (void *)((offset ^ size) + (long long)pool_start);
}

// Remove block from free list - O(1) with prev pointer
static inline void remove_from_free_list(int rank, Block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        // Head of list
        free_list[rank] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
}

// Add block to free list head
static inline void add_to_free_list(int rank, Block *block) {
    block->next = free_list[rank];
    block->prev = NULL;
    if (free_list[rank]) {
        free_list[rank]->prev = block;
    }
    free_list[rank] = block;
}

// Allocate pages
void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return (void *)(long)(-22);  // -EINVAL
    }
    
    int found_rank = rank;
    while (found_rank <= MAX_RANK && free_list[found_rank] == NULL) {
        found_rank++;
    }
    
    if (found_rank > MAX_RANK) {
        return (void *)(long)(-28);  // -ENOSPC
    }
    
    Block *block = free_list[found_rank];
    remove_from_free_list(found_rank, block);
    
    void *result = block->start;
    int start_page = addr_to_page(result);
    
    // Split
    while (found_rank > rank) {
        found_rank--;
        int split_pages = rank_pages(found_rank);
        void *buddy = (char *)result + ((long long)split_pages << 12);
        
        // Add buddy to free list
        Block *buddy_block = (Block *)buddy;
        buddy_block->start = buddy;
        add_to_free_list(found_rank, buddy_block);
        
        // Mark buddy pages
        int buddy_page = start_page + split_pages;
        for (int i = buddy_page; i < buddy_page + split_pages; i++) {
            page_rank[i] = found_rank;
            page_block[i] = buddy_block;
        }
    }
    
    // Mark result as allocated
    int res_page = start_page;
    for (int i = res_page; i < res_page + rank_pages(rank); i++) {
        page_rank[i] = 0;
        page_block[i] = NULL;
    }
    
    AllocMeta *meta = (AllocMeta *)result;
    meta->rank = rank;
    meta->magic = META_MAGIC;
    
    return result;
}

// Return pages
int return_pages(void *p) {
    if (!p) return -22;  // -EINVAL
    
    // Fast validation using page state
    if (!is_in_pool(p)) return -22;
    
    int pg = addr_to_page(p);
    if (page_rank[pg] != 0) return -22;  // Not allocated
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic != META_MAGIC) return -22;
    
    int rank = meta->rank;
    meta->magic = 0;
    
    void *addr = p;
    
    // Coalesce
    while (rank < MAX_RANK) {
        void *buddy = buddy_addr(addr, rank);
        int buddy_pg = addr_to_page(buddy);
        
        // Check if buddy is free at this rank using O(1) lookup
        if (buddy_pg < 0 || buddy_pg >= max_pages) break;
        if (page_rank[buddy_pg] != rank) break;
        
        Block *buddy_block = page_block[buddy_pg];
        if (!buddy_block) break;
        
        // Remove buddy from free list - O(1)
        remove_from_free_list(rank, buddy_block);
        
        // Clear buddy's page entries
        for (int i = buddy_pg; i < buddy_pg + rank_pages(rank); i++) {
            page_rank[i] = 0;
            page_block[i] = NULL;
        }
        
        if (buddy < addr) {
            addr = buddy;
            pg = buddy_pg;
        }
        rank++;
    }
    
    // Add merged block to free list
    Block *block = (Block *)addr;
    block->start = addr;
    add_to_free_list(rank, block);
    
    for (int i = pg; i < pg + rank_pages(rank); i++) {
        page_rank[i] = rank;
        page_block[i] = block;
    }
    
    return 0;
}

// Query rank
int query_ranks(void *p) {
    if (!p) return -22;
    if (!is_in_pool(p)) return -22;
    
    int pg = addr_to_page(p);
    if (pg < 0 || pg >= max_pages) return -22;
    
    if (page_rank[pg] > 0) {
        return page_rank[pg];
    }
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic == META_MAGIC) {
        return meta->rank;
    }
    
    return -22;
}

// Query free page count
int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -22;
    
    int count = 0;
    Block *curr = free_list[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}
