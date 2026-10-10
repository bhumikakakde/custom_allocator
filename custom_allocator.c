/*
 * custom_allocator.c
 *
 * Two custom allocators that manage a caller-supplied buffer (no malloc/free):
 *
 *  1. Pool  - fixed-size blocks. O(1) alloc/free, zero external fragmentation,
 *             fully deterministic. Ideal for real-time systems.
 *  2. Heap  - variable-size blocks. First-fit, block splitting, and coalescing
 *             of neighbours on free to limit fragmentation.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -O2 custom_allocator.c -o allocator_demo
 * Run:    ./allocator_demo
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define ALIGNMENT 16u

static inline size_t align_up(size_t n, size_t a) {
    return (n + a - 1) & ~(a - 1);
}

/* ========================================================================== */
/*  1. FIXED-SIZE POOL ALLOCATOR                                              */
/* ========================================================================== */

typedef struct {
    uint8_t *mem;         /* start of block storage            */
    void    *free_list;   /* singly linked list of free blocks */
    size_t   block_size;  /* aligned size of each block        */
    size_t   block_count;
    size_t   used;
} Pool;

/* Returns 0 on success, -1 if buffer is too small. */
int pool_init(Pool *p, void *buffer, size_t buf_size, size_t block_size) {
    if (!p || !buffer) return -1;

    if (block_size < sizeof(void *)) block_size = sizeof(void *);
    block_size = align_up(block_size, ALIGNMENT);

    uint8_t *start = (uint8_t *)align_up((uintptr_t)buffer, ALIGNMENT);
    size_t   waste = (size_t)(start - (uint8_t *)buffer);
    if (buf_size <= waste) return -1;

    size_t count = (buf_size - waste) / block_size;
    if (count == 0) return -1;

    p->mem = start;
    p->block_size = block_size;
    p->block_count = count;
    p->used = 0;
    p->free_list = NULL;

    /* Thread the free list through the blocks themselves (no extra memory). */
    for (size_t i = count; i-- > 0;) {
        void **blk = (void **)(start + i * block_size);
        *blk = p->free_list;
        p->free_list = blk;
    }
    return 0;
}

void *pool_alloc(Pool *p) {
    if (!p->free_list) return NULL;          /* pool exhausted */
    void **blk = (void **)p->free_list;
    p->free_list = *blk;
    p->used++;
    return blk;
}

void pool_free(Pool *p, void *ptr) {
    if (!ptr) return;
    uint8_t *b = (uint8_t *)ptr;
    /* Sanity: must lie inside the pool and on a block boundary. */
    assert(b >= p->mem && b < p->mem + p->block_count * p->block_size);
    assert(((size_t)(b - p->mem) % p->block_size) == 0);

    *(void **)ptr = p->free_list;
    p->free_list = ptr;
    p->used--;
}

/* ========================================================================== */
/*  2. VARIABLE-SIZE HEAP ALLOCATOR (first-fit, split, coalesce)              */
/* ========================================================================== */

typedef struct Block {
    size_t        size;   /* payload size in bytes (excludes header) */
    struct Block *next;   /* next block in memory order              */
    struct Block *prev;   /* previous block in memory order          */
    uint32_t      free;   /* 1 = free, 0 = allocated                 */
    uint32_t      magic;  /* corruption / double-free detection      */
} Block;

#define HDR_SIZE    align_up(sizeof(Block), ALIGNMENT)
#define MIN_PAYLOAD ALIGNMENT
#define MAGIC_FREE  0xF4EEF4EEu
#define MAGIC_USED  0xA110CA7Eu

typedef struct {
    Block *head;
    size_t total;
} Heap;

int heap_init(Heap *h, void *buffer, size_t buf_size) {
    if (!h || !buffer) return -1;

    uint8_t *start = (uint8_t *)align_up((uintptr_t)buffer, ALIGNMENT);
    size_t   waste = (size_t)(start - (uint8_t *)buffer);
    if (buf_size < waste + HDR_SIZE + MIN_PAYLOAD) return -1;

    Block *b = (Block *)start;
    b->size  = buf_size - waste - HDR_SIZE;
    b->next  = b->prev = NULL;
    b->free  = 1;
    b->magic = MAGIC_FREE;

    h->head  = b;
    h->total = buf_size - waste;
    return 0;
}

static inline void *payload(Block *b) { return (uint8_t *)b + HDR_SIZE; }
static inline Block *header(void *p)  { return (Block *)((uint8_t *)p - HDR_SIZE); }

void *heap_alloc(Heap *h, size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, ALIGNMENT);

    for (Block *b = h->head; b; b = b->next) {
        if (!b->free || b->size < size) continue;

        /* Split if the leftover can hold a header + minimum payload. */
        if (b->size >= size + HDR_SIZE + MIN_PAYLOAD) {
            Block *rest = (Block *)((uint8_t *)payload(b) + size);
            rest->size  = b->size - size - HDR_SIZE;
            rest->free  = 1;
            rest->magic = MAGIC_FREE;
            rest->prev  = b;
            rest->next  = b->next;
            if (b->next) b->next->prev = rest;
            b->next = rest;
            b->size = size;
        }
        b->free  = 0;
        b->magic = MAGIC_USED;
        return payload(b);
    }
    return NULL; /* out of memory */
}

void heap_free(Heap *h, void *ptr) {
    (void)h;
    if (!ptr) return;

    Block *b = header(ptr);
    assert(b->magic == MAGIC_USED && "invalid pointer or double free");
    b->free  = 1;
    b->magic = MAGIC_FREE;

    /* Coalesce with next block. */
    if (b->next && b->next->free) {
        Block *n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next  = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }
    /* Coalesce with previous block. */
    if (b->prev && b->prev->free) {
        Block *p = b->prev;
        p->size += HDR_SIZE + b->size;
        p->next  = b->next;
        if (b->next) b->next->prev = p;
        b->magic = 0;
    }
}

void *heap_calloc(Heap *h, size_t n, size_t sz) {
    if (sz && n > SIZE_MAX / sz) return NULL;   /* overflow check */
    size_t total = n * sz;
    void *p = heap_alloc(h, total);
    if (p) memset(p, 0, total);
    return p;
}

/* Print block layout (handy for seeing fragmentation / coalescing). */
void heap_dump(const Heap *h, const char *title) {
    printf("--- %s ---\n", title);
    size_t used = 0, freeb = 0, i = 0;
    for (Block *b = h->head; b; b = b->next, i++) {
        printf("  [%zu] %-4s %6zu bytes\n", i, b->free ? "FREE" : "USED", b->size);
        if (b->free) freeb += b->size; else used += b->size;
    }
    printf("  used=%zu  free=%zu  (headers=%zu bytes each)\n\n",
           used, freeb, (size_t)HDR_SIZE);
}

/* ========================================================================== */
/*  DEMO                                                                      */
/* ========================================================================== */

typedef struct { int id; float x, y; } Particle;

int main(void) {
    /* ---- Pool demo ---- */
    static uint8_t pool_buf[1024];
    Pool pool;
    if (pool_init(&pool, pool_buf, sizeof pool_buf, sizeof(Particle)) != 0) return 1;
    printf("Pool: %zu blocks of %zu bytes\n", pool.block_count, pool.block_size);

    Particle *a = pool_alloc(&pool);
    Particle *b = pool_alloc(&pool);
    a->id = 1; a->x = 1.5f; a->y = 2.5f;
    b->id = 2; b->x = 3.0f; b->y = 4.0f;
    printf("  allocated 2 -> used=%zu\n", pool.used);
    pool_free(&pool, a);
    Particle *c = pool_alloc(&pool);      /* reuses a's slot instantly */
    printf("  freed a, alloc c reuses slot: %s\n", c == a ? "yes" : "no");
    pool_free(&pool, b);
    pool_free(&pool, c);
    printf("  all freed -> used=%zu\n\n", pool.used);

    /* ---- Heap demo ---- */
    static uint8_t heap_buf[4096];
    Heap heap;
    if (heap_init(&heap, heap_buf, sizeof heap_buf) != 0) return 1;
    heap_dump(&heap, "Fresh heap");

    char *s1 = heap_alloc(&heap, 100);
    int  *s2 = heap_calloc(&heap, 50, sizeof(int));
    char *s3 = heap_alloc(&heap, 300);
    strcpy(s1, "hello, custom allocator!");
    printf("s1 = \"%s\"\n\n", s1);
    heap_dump(&heap, "After 3 allocations");

    heap_free(&heap, s2);
    heap_dump(&heap, "After freeing middle block (hole)");

    heap_free(&heap, s1);
    heap_dump(&heap, "After freeing s1 (coalesces with hole)");

    heap_free(&heap, s3);
    heap_dump(&heap, "After freeing everything (fully coalesced)");

    return 0;
}