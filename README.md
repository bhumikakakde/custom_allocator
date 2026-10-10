# Custom Memory Allocator in C

A small, dependency-free memory allocator written in C. It manages a buffer you
provide instead of calling `malloc()` / `free()`, and includes two strategies:

| Allocator | Block size | Alloc / Free | Fragmentation | Best for |
|-----------|-----------|--------------|---------------|----------|
| `Pool`    | Fixed     | O(1) / O(1)  | None (external) | Real-time systems, many same-sized objects |
| `Heap`    | Variable  | O(n) / O(1)  | Reduced via split + coalesce | General-purpose use inside a fixed region |

## Features

- **Pool allocator**: free list stored inside free blocks (zero metadata overhead), constant-time operations, immediate slot reuse for good cache locality.
- **Heap allocator**: first-fit search, block splitting, and coalescing of adjacent free blocks.
- 16-byte alignment for all returned pointers.
- Magic-number checks that catch double frees and invalid pointers (via `assert`).
- `heap_calloc` with multiplication-overflow protection.
- `heap_dump()` to print the block layout and visualise fragmentation.

## Build and Run

### Windows (MinGW-w64 / WinLibs)

```
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Restart your terminal / VS Code, then:

```
gcc -std=c11 -Wall -Wextra -O2 custom_allocator.c -o allocator_demo.exe
.\allocator_demo.exe
```

### Linux / macOS

```
gcc -std=c11 -Wall -Wextra -O2 custom_allocator.c -o allocator_demo
./allocator_demo
```

## Expected Output (abridged)

```
Pool: 64 blocks of 16 bytes
  allocated 2 -> used=2
  freed a, alloc c reuses slot: yes
  all freed -> used=0

--- Fresh heap ---
  [0] FREE   4064 bytes

--- After 3 allocations ---
  [0] USED    112 bytes
  [1] USED    208 bytes
  [2] USED    304 bytes
  [3] FREE   3344 bytes
...
--- After freeing everything (fully coalesced) ---
  [0] FREE   4064 bytes
```

## API

### Pool

```c
int   pool_init(Pool *p, void *buffer, size_t buf_size, size_t block_size);
void *pool_alloc(Pool *p);
void  pool_free(Pool *p, void *ptr);
```

### Heap

```c
int   heap_init(Heap *h, void *buffer, size_t buf_size);
void *heap_alloc(Heap *h, size_t size);
void *heap_calloc(Heap *h, size_t n, size_t size);
void  heap_free(Heap *h, void *ptr);
void  heap_dump(const Heap *h, const char *title);
```

### Example

```c
static uint8_t buf[4096];
Heap heap;
heap_init(&heap, buf, sizeof buf);

char *s = heap_alloc(&heap, 100);
strcpy(s, "hello");
heap_free(&heap, s);
```

## Design Notes

- **Heap block layout:** each block has a header (`size`, `next`, `prev`, `free`, `magic`) followed by its payload. Blocks are linked in memory order, which makes coalescing neighbours O(1).
- **Splitting:** a free block is split only if the remainder can hold a header plus the minimum payload.
- **Coalescing:** on `heap_free`, the block is merged with its next and previous neighbours if they are free.

## Limitations

- Not thread-safe. Wrap calls in a mutex or use one allocator per thread.
- Heap allocation uses first-fit, so it is not strictly deterministic. Use `Pool` for hard real-time work.
- No `realloc` yet.

## Ideas for Extension

- `heap_realloc`
- Size-class pools layered over the heap
- Segregated free lists or a TLSF allocator for O(1) variable-size allocation
- Arena / bump allocator for per-frame scratch memory
- Thread-safety with a lock

## License

MIT (or your choice).