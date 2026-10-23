#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
size_t heap_caps_get_free_size(unsigned caps);
void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_realloc(void *p, size_t size, unsigned caps);
void heap_caps_free(void *p);
