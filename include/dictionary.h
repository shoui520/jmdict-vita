#ifndef JMDICT_VITA_DICTIONARY_H
#define JMDICT_VITA_DICTIONARY_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

namespace jmdict {

enum {
	QUERY_UTF8_MAX = 512,
	RESULT_MARKUP_MAX = 96 * 1024,
	RESULT_LIMIT = 20
};

struct SearchStats {
	uint32_t results;
	uint32_t index_rows_read;
	bool truncated;
};

struct Engine {
	SceUID fd;
	uint32_t entry_count;
	uint32_t key_count;
	uint32_t key_block_count;
	uint32_t entry_block_count;
	uint32_t keys_per_block;
	uint32_t max_key_bytes;
	uint32_t max_postings;
	uint32_t max_entry_size;
	uint32_t max_block_raw;
	uint32_t max_block_stored;
	uint32_t cached_block;
	uint32_t cached_first_entry;
	uint32_t cached_entry_count;
	uint32_t cached_raw_size;
	uint64_t index_blocks_offset;
	uint64_t index_stream_offset;
	uint64_t entry_blocks_offset;
	uint64_t entry_data_offset;
	uint64_t file_size;
	void *stored_buffer;
	void *block_buffer;
	uint32_t *candidate_buffer;
};

void Reset(Engine *engine);
int Open(Engine *engine, const char *path);
void Close(Engine *engine);

// Converts PAF UTF-16 to an index query. ASCII word boundaries are retained
// for compact multi-token English lookup; Japanese spacing is removed.
size_t NormalizeQuery(const wchar_t *input, size_t length, char *output, size_t capacity);

// Performs lower-bound/prefix lookup and writes PAF RichText-safe markup.
int Search(Engine *engine, const char *query, char *markup, size_t capacity, SearchStats *stats);

} // namespace jmdict

#endif
