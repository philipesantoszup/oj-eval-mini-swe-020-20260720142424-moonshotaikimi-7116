#include "buddy.h"

#define NULL ((void *)0)
#define MAX_RANK 16
#define MAX_PAGES 300000

typedef struct Block {
    void *start;
    struct Block *next;
    struct Block *prev;
} Block;

#define META_MAGIC 0xDEADBEEF

typedef struct {
    int rank;
    int magic;
} AllocMeta;

static void *pool_start = NULL;
static int pool_pgcount = 0;
static int max_pages = 0;

// O(1) lookup arrays
static unsigned char page_rank[MAX_PAGES];
static Block *page_block[MAX_PAGES];
static int free_count[MAX_RANK + 1];  // Cache free block counts

// Free list heads
static Block *free_list[MAX_RANK + 1];

static inline int addr_to_page(void *p) {
    return (int)(((long long)p - (long long)pool_start) >> 12);
}

static inline int rank_pages(int rank) {
    return 1 << (rank - 1);
}

static inline int is_in_pool(void *p) {
    int idx = addr_to_page(p);
    return idx >= 0 && idx < max_pages;
}

// Initialize
int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) return -22; // EINVAL
    
    pool_start = p;
    pool_pgcount = pgcount;
    max_pages = pgcount;
    
    // Clear all structures
    for (int i = 0; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
        free_count[i] = 0;
    }
    for (int i = 0; i < max_pages; i++) {
        page_rank[i] = 0;
        page_block[i] = NULL;
    }
    
    // Build free blocks
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
        block->next = NULL;
        block->prev = NULL;
        
        // Add to head
        if (free_list[rank]) {
            free_list[rank]->prev = block;
        }
        block->next = free_list[rank];
        free_list[rank] = block;
        free_count[rank]++;
        
        for (int i = pos; i < pos + pages && i < max_pages; i++) {
            page_rank[i] = rank;
            page_block[i] = block;
        }
        
        pos += pages;
    }
    
    return 0;
}

// Buddy address
static inline void *buddy_addr(void *p, int rank) {
    long long offset = (long long)p - (long long)pool_start;
    long long size = (long long)rank_pages(rank) << 12;
    return (void *)((offset ^ size) + (long long)pool_start);
}

// Remove from free list
static inline void remove_free(int rank, Block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list[rank] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    free_count[rank]--;
}

// Add to free list head
static inline void add_free(int rank, Block *block) {
    block->prev = NULL;
    block->next = free_list[rank];
    if (free_list[rank]) {
        free_list[rank]->prev = block;
    }
    free_list[rank] = block;
    free_count[rank]++;
}

// Allocate
void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return (void *)(long)(-22);
    }
    
    int found = rank;
    while (found <= MAX_RANK && free_list[found] == NULL) {
        found++;
    }
    
    if (found > MAX_RANK) {
        return (void *)(long)(-28);
    }
    
    Block *block = free_list[found];
    remove_free(found, block);
    
    void *res = block->start;
    int start_pg = addr_to_page(res);
    
    // Split
    while (found > rank) {
        found--;
        int split_pages = rank_pages(found);
        void *buddy = (char *)res + ((long long)split_pages << 12);
        
        Block *bblock = (Block *)buddy;
        bblock->start = buddy;
        add_free(found, bblock);
        
        int buddy_pg = start_pg + split_pages;
        for (int i = buddy_pg; i < buddy_pg + split_pages; i++) {
            page_rank[i] = found;
            page_block[i] = bblock;
        }
    }
    
    // Mark allocated
    for (int i = start_pg; i < start_pg + rank_pages(rank); i++) {
        page_rank[i] = 0;
        page_block[i] = NULL;
    }
    
    AllocMeta *meta = (AllocMeta *)res;
    meta->rank = rank;
    meta->magic = META_MAGIC;
    
    return res;
}

// Return
int return_pages(void *p) {
    if (!p) return -22;
    if (!is_in_pool(p)) return -22;
    
    int pg = addr_to_page(p);
    if (page_rank[pg] != 0) return -22;
    
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic != META_MAGIC) return -22;
    
    int rank = meta->rank;
    meta->magic = 0;
    
    void *addr = p;
    int start_pg = pg;
    
    // Coalesce
    while (rank < MAX_RANK) {
        void *buddy = buddy_addr(addr, rank);
        int buddy_pg = addr_to_page(buddy);
        
        if (buddy_pg < 0 || buddy_pg >= max_pages) break;
        if (page_rank[buddy_pg] != rank) break;
        
        Block *bblock = page_block[buddy_pg];
        if (!bblock) break;
        
        remove_free(rank, bblock);
        
        // Clear buddy pages
        for (int i = buddy_pg; i < buddy_pg + rank_pages(rank); i++) {
            page_rank[i] = 0;
            page_block[i] = NULL;
        }
        
        if (buddy < addr) {
            addr = buddy;
            start_pg = buddy_pg;
        }
        rank++;
    }
    
    // Add merged block
    Block *block = (Block *)addr;
    block->start = addr;
    add_free(rank, block);
    
    for (int i = start_pg; i < start_pg + rank_pages(rank); i++) {
        page_rank[i] = rank;
        page_block[i] = block;
    }
    
    return 0;
}

// Query rank - O(1) lookup
int query_ranks(void *p) {
    if (!p) return -22;
    if (!is_in_pool(p)) return -22;
    
    int pg = addr_to_page(p);
    if (pg < 0 || pg >= max_pages) return -22;
    
    // Free block
    if (page_rank[pg] > 0) {
        return page_rank[pg];
    }
    
    // Allocated
    AllocMeta *meta = (AllocMeta *)p;
    if (meta->magic == META_MAGIC) {
        return meta->rank;
    }
    
    return -22;
}

// Query free count - O(1) using cached count
int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -22;
    return free_count[rank];
}
