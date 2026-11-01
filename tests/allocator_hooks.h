#pragma once
#include <stdlib.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* audio_test_malloc(size_t);
void* audio_test_calloc(size_t, size_t);
void* audio_test_realloc(void*, size_t);
void audio_test_free(void*);
void audio_test_track(int enabled);
size_t audio_test_allocations(void);
#ifdef __cplusplus
}
#else
#define malloc audio_test_malloc
#define calloc audio_test_calloc
#define realloc audio_test_realloc
#define free audio_test_free
#endif
