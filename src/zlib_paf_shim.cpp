#include <stddef.h>
#include <paf/std/stdlib.h>
#include <paf/std/string.h>

// Core zlib is used only after ScePaf is loaded.  Route its allocator and
// byte primitives into the PAF heap so this process image does not pull in a
// second CRT heap.
extern "C" {

void *malloc(size_t size) { return sce_paf_malloc(size); }
void free(void *pointer) { sce_paf_free(pointer); }
void *memcpy(void *destination, const void *source, size_t size) {
	return sce_paf_memcpy(destination, source, size);
}
void *memmove(void *destination, const void *source, size_t size) {
	return sce_paf_memmove(destination, source, size);
}
void *memset(void *destination, int value, size_t size) {
	return sce_paf_memset(destination, value, size);
}

}
