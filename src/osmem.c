// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT		8
#define ALIGN(x)		(((x) + (ALIGNMENT - 1)) & ~((size_t)ALIGNMENT - 1))
#define META_SIZE		ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD		(128 * 1024)
#define PREALLOC_SIZE		(128 * 1024)

#define BLOCK_PAYLOAD(b)	((void *)((char *)(b) + META_SIZE))
#define PAYLOAD_BLOCK(p)	((struct block_meta *)((char *)(p) - META_SIZE))

/* Head of the list of blocks living on the heap (ordered by address). */
static struct block_meta *heap_head;
static struct block_meta *heap_tail;

static void *heap_grow(size_t bytes)
{
	void *ret = sbrk(bytes);

	DIE(ret == (void *)-1, "sbrk");
	return ret;
}

static void list_append(struct block_meta *block)
{
	block->next = NULL;
	block->prev = heap_tail;
	if (heap_tail)
		heap_tail->next = block;
	else
		heap_head = block;
	heap_tail = block;
}

/* Merge a free block with its successor (which must be free as well). */
static void merge_with_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;
	if (next->next)
		next->next->prev = block;
	else
		heap_tail = block;
}

/* Merge every run of adjacent free blocks. */
static void coalesce_all(void)
{
	struct block_meta *cur = heap_head;

	while (cur) {
		if (cur->status == STATUS_FREE && cur->next &&
		    cur->next->status == STATUS_FREE)
			merge_with_next(cur);
		else
			cur = cur->next;
	}
}

/* Shrink `block` to `size` bytes, turning the rest into a free block. */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + ALIGNMENT)
		return;

	rest = (struct block_meta *)((char *)block + META_SIZE + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;
	if (block->next)
		block->next->prev = rest;
	else
		heap_tail = rest;
	block->next = rest;
	block->size = size;
}

static struct block_meta *find_best(size_t size)
{
	struct block_meta *cur, *best = NULL;

	for (cur = heap_head; cur; cur = cur->next)
		if (cur->status == STATUS_FREE && cur->size >= size &&
		    (!best || cur->size < best->size))
			best = cur;
	return best;
}

static void *map_block(size_t size)
{
	struct block_meta *block;
	void *addr;

	addr = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	DIE(addr == MAP_FAILED, "mmap");

	block = addr;
	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;
	return BLOCK_PAYLOAD(block);
}

static void *heap_alloc(size_t size)
{
	struct block_meta *block, *new_block;

	/* Heap preallocation: happens only once. */
	if (!heap_head) {
		block = heap_grow(PREALLOC_SIZE);
		block->size = PREALLOC_SIZE - META_SIZE;
		block->status = STATUS_FREE;
		list_append(block);
	}

	coalesce_all();

	block = find_best(size);
	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return BLOCK_PAYLOAD(block);
	}

	/* Not enough room: expand the last block if free, else add a new one. */
	if (heap_tail->status == STATUS_FREE) {
		heap_grow(size - heap_tail->size);
		heap_tail->size = size;
		heap_tail->status = STATUS_ALLOC;
		return BLOCK_PAYLOAD(heap_tail);
	}

	new_block = heap_grow(META_SIZE + size);
	new_block->size = size;
	new_block->status = STATUS_ALLOC;
	list_append(new_block);
	return BLOCK_PAYLOAD(new_block);
}

static void *do_alloc(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);
	if (META_SIZE + size >= threshold)
		return map_block(size);
	return heap_alloc(size);
}

void *os_malloc(size_t size)
{
	return do_alloc(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	struct block_meta *block;
	int ret;

	if (!ptr)
		return;

	block = PAYLOAD_BLOCK(ptr);
	if (block->status == STATUS_MAPPED) {
		ret = munmap(block, META_SIZE + block->size);
		DIE(ret == -1, "munmap");
	} else if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;
	if (__builtin_mul_overflow(nmemb, size, &total))
		return NULL;

	ptr = do_alloc(total, (size_t)getpagesize());
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}

/* Allocate a new block, copy the payload over and release the old block. */
static void *realloc_move(struct block_meta *block, size_t size)
{
	void *new_ptr = os_malloc(size);

	if (!new_ptr)
		return NULL;
	memcpy(new_ptr, BLOCK_PAYLOAD(block),
	       block->size < size ? block->size : size);
	os_free(BLOCK_PAYLOAD(block));
	return new_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;

	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	block = PAYLOAD_BLOCK(ptr);
	if (block->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);

	if (block->status == STATUS_MAPPED || META_SIZE + size >= MMAP_THRESHOLD)
		return realloc_move(block, size);

	/* Shrink (or keep) in place. */
	if (size <= block->size) {
		split_block(block, size);
		return ptr;
	}

	/* Try to grow by absorbing the following free blocks, one at a time. */
	while (block->next && block->next->status == STATUS_FREE &&
	       block->size < size)
		merge_with_next(block);

	if (block->size >= size) {
		split_block(block, size);
		return ptr;
	}

	/* Last block on the heap: just extend the heap. */
	if (block == heap_tail) {
		heap_grow(size - block->size);
		block->size = size;
		return ptr;
	}

	return realloc_move(block, size);
}
