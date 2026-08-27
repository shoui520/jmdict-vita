#include "dictionary.h"

#include <limits>
#include <paf.h>
#include <psp2/io/fcntl.h>
#include <zlib.h>

namespace jmdict {
namespace {

static const uint32_t kFormatVersion = 3;
static const uint32_t kHeaderSize = 112;
static const uint32_t kKeysPerBlock = 64;
static const uint32_t kKeyReadLimit = 511;
static const uint32_t kEntryReadLimit = 48 * 1024;
static const uint32_t kBlockReadLimit = 64 * 1024;
static const uint32_t kPostingReadLimit = 64 * 1024;
static const uint32_t kEnglishTokenLimit = 4;
static const size_t kCursorBufferSize = 1024;

struct __attribute__((packed)) FileHeader {
	char magic[8];
	uint32_t version;
	uint32_t header_size;
	uint32_t flags;
	uint32_t entry_count;
	uint32_t jmdict_count;
	uint32_t jmnedict_count;
	uint32_t key_count;
	uint32_t key_block_count;
	uint32_t entry_block_count;
	uint32_t keys_per_block;
	uint32_t max_key_bytes;
	uint32_t max_postings;
	uint32_t max_entry_size;
	uint32_t max_block_raw;
	uint32_t max_block_stored;
	uint32_t reserved;
	uint64_t index_blocks_offset;
	uint64_t index_stream_offset;
	uint64_t entry_blocks_offset;
	uint64_t entry_data_offset;
	uint64_t file_size;
};

struct __attribute__((packed)) KeyBlockRecord {
	uint32_t stream_offset;
};

struct __attribute__((packed)) EntryBlockRecord {
	uint32_t first_entry;
	uint32_t data_offset;
	uint32_t stored_length;
	uint32_t raw_length;
	uint32_t entry_count;
};

struct PostingView {
	uint64_t offset;
	uint32_t count;
};

static_assert(sizeof(FileHeader) == kHeaderSize, "dictionary header layout changed");
static_assert(sizeof(KeyBlockRecord) == 4, "key block layout changed");
static_assert(sizeof(EntryBlockRecord) == 20, "entry block layout changed");

class Appender {
public:
	Appender(char *buffer, size_t capacity)
		: buffer_(buffer), capacity_(capacity), size_(0), truncated_(false) {
		if (capacity_) buffer_[0] = 0;
	}

	void Add(const char *text) { Add(text, sce_paf_strlen(text)); }

	void Add(const char *text, size_t length) {
		if (!capacity_ || !length) return;
		size_t available = capacity_ - 1 - size_;
		if (length > available) {
			length = available;
			truncated_ = true;
		}
		if (length) {
			sce_paf_memcpy(buffer_ + size_, text, length);
			size_ += length;
			buffer_[size_] = 0;
		}
	}

	size_t Remaining() const { return capacity_ ? capacity_ - 1 - size_ : 0; }
	bool Truncated() const { return truncated_; }
	void MarkTruncated() { truncated_ = true; }

private:
	char *buffer_;
	size_t capacity_;
	size_t size_;
	bool truncated_;
};

bool ReadAt(SceUID fd, uint64_t offset, void *buffer, size_t length) {
	if (sceIoLseek(fd, static_cast<SceOff>(offset), SCE_SEEK_SET) < 0) return false;
	uint8_t *out = static_cast<uint8_t *>(buffer);
	size_t done = 0;
	while (done < length) {
		int read = sceIoRead(fd, out + done, length - done);
		if (read <= 0) return false;
		done += static_cast<size_t>(read);
	}
	return true;
}

class IndexCursor {
public:
	IndexCursor(const Engine *engine, uint64_t position)
		: engine_(engine), position_(position), used_(0), available_(0) {}

	bool ReadByte(uint8_t *value) {
		if (used_ == available_ && !Fill()) return false;
		*value = buffer_[used_++];
		++position_;
		return true;
	}

	bool Read(void *destination, size_t length) {
		uint8_t *out = static_cast<uint8_t *>(destination);
		while (length) {
			if (used_ == available_ && !Fill()) return false;
			size_t chunk = available_ - used_;
			if (chunk > length) chunk = length;
			sce_paf_memcpy(out, buffer_ + used_, chunk);
			out += chunk;
			used_ += chunk;
			position_ += chunk;
			length -= chunk;
		}
		return true;
	}

	uint64_t Position() const { return position_; }

private:
	bool Fill() {
		if (position_ >= engine_->entry_blocks_offset) return false;
		uint64_t remaining = engine_->entry_blocks_offset - position_;
		size_t amount = remaining < sizeof(buffer_) ? static_cast<size_t>(remaining) : sizeof(buffer_);
		if (!amount || !ReadAt(engine_->fd, position_, buffer_, amount)) return false;
		used_ = 0;
		available_ = amount;
		return true;
	}

	const Engine *engine_;
	uint64_t position_;
	size_t used_;
	size_t available_;
	uint8_t buffer_[kCursorBufferSize];
};

bool ReadUleb(IndexCursor *cursor, uint32_t *value) {
	uint32_t result = 0;
	for (uint32_t index = 0; index < 5; ++index) {
		uint8_t byte;
		if (!cursor->ReadByte(&byte)) return false;
		if (index == 4 && (byte & 0xF0)) return false;
		result |= static_cast<uint32_t>(byte & 0x7F) << (index * 7);
		if (!(byte & 0x80)) {
			*value = result;
			return true;
		}
	}
	return false;
}

bool DecodeKey(const Engine *engine, IndexCursor *cursor, char *key, size_t previous_length,
	size_t *key_length, uint32_t *posting_count) {
	uint32_t prefix;
	uint32_t suffix;
	uint32_t postings;
	if (!ReadUleb(cursor, &prefix) || !ReadUleb(cursor, &suffix) || !ReadUleb(cursor, &postings))
		return false;
	if (prefix > previous_length || prefix > engine->max_key_bytes ||
		suffix > engine->max_key_bytes - prefix || prefix > kKeyReadLimit ||
		suffix > kKeyReadLimit - prefix || postings > engine->max_postings) return false;
	if (!cursor->Read(key + prefix, suffix)) return false;
	*key_length = prefix + suffix;
	key[*key_length] = 0;
	*posting_count = postings;
	return true;
}

bool SkipPostings(IndexCursor *cursor, uint32_t count) {
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t ignored;
		if (!ReadUleb(cursor, &ignored)) return false;
	}
	return true;
}

int CompareBytes(const char *left, size_t left_length, const char *right, size_t right_length) {
	size_t common = left_length < right_length ? left_length : right_length;
	int result = sce_paf_memcmp(left, right, common);
	if (result != 0) return result;
	if (left_length < right_length) return -1;
	if (left_length > right_length) return 1;
	return 0;
}

bool HasPrefix(const char *key, size_t key_length, const char *query, size_t query_length) {
	return key_length >= query_length && sce_paf_memcmp(key, query, query_length) == 0;
}

bool HeaderMagicValid(const FileHeader& header) {
	static const char expected[8] = {'J', 'M', 'D', 'V', 'I', 'T', 'A', '1'};
	return sce_paf_memcmp(header.magic, expected, sizeof(expected)) == 0;
}

bool ReadKeyBlock(const Engine *engine, uint32_t block, KeyBlockRecord *record) {
	if (block >= engine->key_block_count) return false;
	return ReadAt(engine->fd, engine->index_blocks_offset +
		static_cast<uint64_t>(block) * sizeof(*record), record, sizeof(*record));
}

bool ReadEntryBlock(const Engine *engine, uint32_t block, EntryBlockRecord *record) {
	if (block >= engine->entry_block_count) return false;
	return ReadAt(engine->fd, engine->entry_blocks_offset +
		static_cast<uint64_t>(block) * sizeof(*record), record, sizeof(*record));
}

bool ReadFirstKey(const Engine *engine, uint32_t block, char *key, size_t *length) {
	KeyBlockRecord record;
	if (!ReadKeyBlock(engine, block, &record)) return false;
	IndexCursor cursor(engine, engine->index_stream_offset + record.stream_offset);
	uint32_t postings;
	return DecodeKey(engine, &cursor, key, 0, length, &postings);
}

bool FindStartBlock(const Engine *engine, const char *query, size_t query_length, uint32_t *block) {
	if (!engine->key_block_count) return false;
	uint32_t low = 0;
	uint32_t high = engine->key_block_count;
	char key[kKeyReadLimit + 1];
	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		size_t key_length;
		if (!ReadFirstKey(engine, middle, key, &key_length)) return false;
		if (CompareBytes(key, key_length, query, query_length) <= 0) low = middle + 1;
		else high = middle;
	}
	*block = low == 0 ? 0 : low - 1;
	return true;
}

bool FindExactPostings(const Engine *engine, const char *query, size_t query_length,
	PostingView *view, SearchStats *stats) {
	uint32_t block;
	if (!FindStartBlock(engine, query, query_length, &block)) return false;
	KeyBlockRecord block_record;
	if (!ReadKeyBlock(engine, block, &block_record)) return false;
	IndexCursor cursor(engine, engine->index_stream_offset + block_record.stream_offset);
	uint32_t key_index = block * engine->keys_per_block;
	char key[kKeyReadLimit + 1];
	size_t key_length = 0;
	while (key_index < engine->key_count) {
		if (key_index % engine->keys_per_block == 0) key_length = 0;
		uint32_t posting_count;
		if (!DecodeKey(engine, &cursor, key, key_length, &key_length, &posting_count)) return false;
		++stats->index_rows_read;
		int compare = CompareBytes(key, key_length, query, query_length);
		if (compare == 0) {
			view->offset = cursor.Position();
			view->count = posting_count;
			return true;
		}
		if (compare > 0) return false;
		if (!SkipPostings(&cursor, posting_count)) return false;
		++key_index;
	}
	return false;
}

bool LoadPostingView(const Engine *engine, const PostingView& view, uint32_t *output,
	uint32_t *count) {
	if (view.count > engine->max_postings) return false;
	IndexCursor cursor(engine, view.offset);
	uint32_t previous = 0;
	for (uint32_t index = 0; index < view.count; ++index) {
		uint32_t entry_id;
		if (!ReadUleb(&cursor, &entry_id) || entry_id >= engine->entry_count) return false;
		if (index && entry_id <= previous) return false;
		output[index] = entry_id;
		previous = entry_id;
	}
	*count = view.count;
	return true;
}

bool IntersectPostingView(const Engine *engine, const PostingView& view, uint32_t *candidates,
	uint32_t *candidate_count) {
	IndexCursor cursor(engine, view.offset);
	uint32_t candidate_index = 0;
	uint32_t output_count = 0;
	uint32_t previous = 0;
	for (uint32_t index = 0; index < view.count && candidate_index < *candidate_count; ++index) {
		uint32_t entry_id;
		if (!ReadUleb(&cursor, &entry_id) || entry_id >= engine->entry_count) return false;
		if (index && entry_id <= previous) return false;
		previous = entry_id;
		while (candidate_index < *candidate_count && candidates[candidate_index] < entry_id)
			++candidate_index;
		if (candidate_index < *candidate_count && candidates[candidate_index] == entry_id) {
			candidates[output_count++] = entry_id;
			++candidate_index;
		}
	}
	*candidate_count = output_count;
	return true;
}

bool LoadEntryBlock(Engine *engine, uint32_t block, const EntryBlockRecord& record) {
	if (!record.entry_count || !record.raw_length || !record.stored_length ||
		record.raw_length > engine->max_block_raw || record.stored_length > engine->max_block_stored)
		return false;
	if (!ReadAt(engine->fd, engine->entry_data_offset + record.data_offset,
		engine->stored_buffer, record.stored_length)) return false;
	if (record.stored_length == record.raw_length) {
		sce_paf_memcpy(engine->block_buffer, engine->stored_buffer, record.raw_length);
	} else {
		uLongf output_length = record.raw_length;
		int result = uncompress(reinterpret_cast<Bytef *>(engine->block_buffer), &output_length,
			reinterpret_cast<const Bytef *>(engine->stored_buffer), record.stored_length);
		if (result != Z_OK || output_length != record.raw_length) return false;
	}
	engine->cached_block = block;
	engine->cached_first_entry = record.first_entry;
	engine->cached_entry_count = record.entry_count;
	engine->cached_raw_size = record.raw_length;
	return true;
}

bool FindEntryBlock(Engine *engine, uint32_t entry_id, uint32_t *block, EntryBlockRecord *record) {
	if (engine->cached_block != 0xFFFFFFFFu && entry_id >= engine->cached_first_entry &&
		entry_id - engine->cached_first_entry < engine->cached_entry_count) {
		*block = engine->cached_block;
		return ReadEntryBlock(engine, *block, record);
	}
	uint32_t low = 0;
	uint32_t high = engine->entry_block_count;
	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		EntryBlockRecord candidate;
		if (!ReadEntryBlock(engine, middle, &candidate)) return false;
		if (candidate.first_entry <= entry_id) low = middle + 1;
		else high = middle;
	}
	if (!low) return false;
	*block = low - 1;
	return ReadEntryBlock(engine, *block, record);
}

bool AppendEntry(Engine *engine, uint32_t entry_id, Appender *out) {
	if (entry_id >= engine->entry_count) return false;
	uint32_t block;
	EntryBlockRecord record;
	if (!FindEntryBlock(engine, entry_id, &block, &record)) return false;
	if (block != engine->cached_block && !LoadEntryBlock(engine, block, record)) return false;
	uint32_t relative = entry_id - engine->cached_first_entry;
	if (relative >= engine->cached_entry_count) return false;
	const uint8_t *data = static_cast<const uint8_t *>(engine->block_buffer);
	uint32_t offset = 0;
	for (uint32_t index = 0; index <= relative; ++index) {
		if (offset + 2 > engine->cached_raw_size) return false;
		uint32_t length = data[offset] | static_cast<uint32_t>(data[offset + 1]) << 8;
		offset += 2;
		if (!length || length > engine->max_entry_size || offset + length > engine->cached_raw_size)
			return false;
		if (index == relative) {
			if (out->Remaining() < length + 64) {
				out->MarkTruncated();
				return false;
			}
			out->Add(reinterpret_cast<const char *>(data + offset), length);
			out->Add("<br/>");
			return true;
		}
		offset += length;
	}
	return false;
}

void AddEscaped(Appender *out, const char *text) {
	for (const char *p = text; *p; ++p) {
		switch (*p) {
		case '&': out->Add("&amp;"); break;
		case '<': out->Add("&lt;"); break;
		case '>': out->Add("&gt;"); break;
		case '"': out->Add("&quot;"); break;
		default: out->Add(p, 1); break;
		}
	}
}

bool EntryAlreadyUsed(const uint32_t *entries, uint32_t count, uint32_t entry_id) {
	for (uint32_t index = 0; index < count; ++index) {
		if (entries[index] == entry_id) return true;
	}
	return false;
}

bool SearchPrefix(Engine *engine, const char *query, Appender *out, SearchStats *stats) {
	size_t query_length = sce_paf_strlen(query);
	uint32_t block;
	if (!FindStartBlock(engine, query, query_length, &block)) return false;
	KeyBlockRecord block_record;
	if (!ReadKeyBlock(engine, block, &block_record)) return false;
	IndexCursor cursor(engine, engine->index_stream_offset + block_record.stream_offset);
	uint32_t key_index = block * engine->keys_per_block;
	char key[kKeyReadLimit + 1];
	size_t key_length = 0;
	uint32_t used_entries[RESULT_LIMIT];
	while (key_index < engine->key_count && stats->results < RESULT_LIMIT) {
		if (key_index % engine->keys_per_block == 0) key_length = 0;
		uint32_t posting_count;
		if (!DecodeKey(engine, &cursor, key, key_length, &key_length, &posting_count)) return false;
		++stats->index_rows_read;
		int compare = CompareBytes(key, key_length, query, query_length);
		if (compare < 0) {
			if (!SkipPostings(&cursor, posting_count)) return false;
			++key_index;
			continue;
		}
		if (!HasPrefix(key, key_length, query, query_length)) break;
		for (uint32_t index = 0; index < posting_count; ++index) {
			uint32_t entry_id;
			if (!ReadUleb(&cursor, &entry_id) || entry_id >= engine->entry_count) return false;
			if (EntryAlreadyUsed(used_entries, stats->results, entry_id)) continue;
			if (!AppendEntry(engine, entry_id, out)) {
				if (out->Truncated()) return true;
				continue;
			}
			used_entries[stats->results++] = entry_id;
			if (stats->results >= RESULT_LIMIT) return true;
		}
		++key_index;
	}
	return true;
}

bool IsStopWord(const char *token) {
	static const char *words[] = {
		"a", "an", "and", "as", "at", "be", "by", "for", "from", "in",
		"is", "of", "on", "one", "or", "someone", "something", "the", "to", "with"
	};
	for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); ++index) {
		if (sce_paf_strcmp(token, words[index]) == 0) return true;
	}
	return false;
}

uint32_t SplitEnglishTokens(const char *query, char tokens[kEnglishTokenLimit][QUERY_UTF8_MAX]) {
	uint32_t count = 0;
	const char *start = query;
	while (*start && count < kEnglishTokenLimit) {
		while (*start == ' ') ++start;
		if (!*start) break;
		const char *end = start;
		while (*end && *end != ' ') ++end;
		size_t length = static_cast<size_t>(end - start);
		if (length && length < QUERY_UTF8_MAX) {
			sce_paf_memcpy(tokens[count], start, length);
			tokens[count][length] = 0;
			if (!IsStopWord(tokens[count])) {
				bool duplicate = false;
				for (uint32_t index = 0; index < count; ++index) {
					if (sce_paf_strcmp(tokens[index], tokens[count]) == 0) duplicate = true;
				}
				if (!duplicate) ++count;
			}
		}
		start = end;
	}
	return count;
}

bool SearchMultipleEnglishTokens(Engine *engine,
	char tokens[kEnglishTokenLimit][QUERY_UTF8_MAX], uint32_t token_count,
	Appender *out, SearchStats *stats) {
	PostingView views[kEnglishTokenLimit];
	uint32_t smallest = 0;
	for (uint32_t index = 0; index < token_count; ++index) {
		if (!FindExactPostings(engine, tokens[index], sce_paf_strlen(tokens[index]), &views[index], stats))
			return true;
		if (index == 0 || views[index].count < views[smallest].count) smallest = index;
	}
	uint32_t candidate_count;
	if (!LoadPostingView(engine, views[smallest], engine->candidate_buffer, &candidate_count)) return false;
	for (uint32_t index = 0; index < token_count && candidate_count; ++index) {
		if (index == smallest) continue;
		if (!IntersectPostingView(engine, views[index], engine->candidate_buffer, &candidate_count))
			return false;
	}
	for (uint32_t index = 0; index < candidate_count && stats->results < RESULT_LIMIT; ++index) {
		if (!AppendEntry(engine, engine->candidate_buffer[index], out)) {
			if (out->Truncated()) break;
			continue;
		}
		++stats->results;
	}
	return true;
}

void EncodeUtf8(uint32_t codepoint, char *output, size_t capacity, size_t *used) {
	char bytes[4];
	size_t count = 0;
	if (codepoint <= 0x7F) {
		bytes[count++] = static_cast<char>(codepoint);
	} else if (codepoint <= 0x7FF) {
		bytes[count++] = static_cast<char>(0xC0 | (codepoint >> 6));
		bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
	} else if (codepoint <= 0xFFFF) {
		bytes[count++] = static_cast<char>(0xE0 | (codepoint >> 12));
		bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
	} else {
		bytes[count++] = static_cast<char>(0xF0 | (codepoint >> 18));
		bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
	}
	if (*used + count >= capacity) return;
	sce_paf_memcpy(output + *used, bytes, count);
	*used += count;
}

bool SkipAscii(uint32_t codepoint) {
	return (codepoint >= 0x21 && codepoint <= 0x2F) ||
		(codepoint >= 0x3A && codepoint <= 0x40) ||
		(codepoint >= 0x5B && codepoint <= 0x60) ||
		(codepoint >= 0x7B && codepoint <= 0x7E);
}

uint32_t Utf16Codepoint(const wchar_t *input, size_t length, size_t *index) {
	uint32_t codepoint = static_cast<uint16_t>(input[*index]);
	if (codepoint >= 0xD800 && codepoint <= 0xDBFF && *index + 1 < length) {
		uint32_t low = static_cast<uint16_t>(input[*index + 1]);
		if (low >= 0xDC00 && low <= 0xDFFF) {
			codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
			++*index;
		}
	}
	return codepoint;
}

} // namespace

void Reset(Engine *engine) {
	if (!engine) return;
	sce_paf_memset(engine, 0, sizeof(*engine));
	engine->fd = -1;
	engine->cached_block = 0xFFFFFFFFu;
}

int Open(Engine *engine, const char *path) {
	if (!engine || !path) return -1;
	Reset(engine);
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return fd;

	FileHeader header;
	if (!ReadAt(fd, 0, &header, sizeof(header))) {
		sceIoClose(fd);
		return -2;
	}
	SceOff actual_size = sceIoLseek(fd, 0, SCE_SEEK_END);
	uint64_t expected_index_stream = kHeaderSize +
		static_cast<uint64_t>(header.key_block_count) * sizeof(KeyBlockRecord);
	uint64_t expected_entry_data = header.entry_blocks_offset +
		static_cast<uint64_t>(header.entry_block_count) * sizeof(EntryBlockRecord);
	uint32_t expected_key_blocks = header.key_count ?
		(header.key_count + kKeysPerBlock - 1) / kKeysPerBlock : 0;
	bool layout_valid = HeaderMagicValid(header) && header.version == kFormatVersion &&
		header.header_size == kHeaderSize && header.entry_count == header.jmdict_count + header.jmnedict_count &&
		header.keys_per_block == kKeysPerBlock && header.key_block_count == expected_key_blocks &&
		header.index_blocks_offset == kHeaderSize && header.index_stream_offset == expected_index_stream &&
		header.entry_blocks_offset >= header.index_stream_offset && header.entry_data_offset == expected_entry_data &&
		header.entry_data_offset <= header.file_size && actual_size >= 0 &&
		header.file_size == static_cast<uint64_t>(actual_size) &&
		header.max_key_bytes > 0 && header.max_key_bytes <= kKeyReadLimit &&
		header.max_postings > 0 && header.max_postings <= kPostingReadLimit &&
		header.max_entry_size > 0 && header.max_entry_size <= kEntryReadLimit &&
		header.max_block_raw > 0 && header.max_block_raw <= kBlockReadLimit &&
		header.max_block_stored > 0 && header.max_block_stored <= kBlockReadLimit;
	if (!layout_valid) {
		sceIoClose(fd);
		return -3;
	}

	engine->fd = fd;
	engine->entry_count = header.entry_count;
	engine->key_count = header.key_count;
	engine->key_block_count = header.key_block_count;
	engine->entry_block_count = header.entry_block_count;
	engine->keys_per_block = header.keys_per_block;
	engine->max_key_bytes = header.max_key_bytes;
	engine->max_postings = header.max_postings;
	engine->max_entry_size = header.max_entry_size;
	engine->max_block_raw = header.max_block_raw;
	engine->max_block_stored = header.max_block_stored;
	engine->index_blocks_offset = header.index_blocks_offset;
	engine->index_stream_offset = header.index_stream_offset;
	engine->entry_blocks_offset = header.entry_blocks_offset;
	engine->entry_data_offset = header.entry_data_offset;
	engine->file_size = header.file_size;
	engine->cached_block = 0xFFFFFFFFu;
	engine->stored_buffer = sce_paf_malloc(engine->max_block_stored);
	engine->block_buffer = sce_paf_malloc(engine->max_block_raw);
	engine->candidate_buffer = static_cast<uint32_t *>(
		sce_paf_malloc(static_cast<size_t>(engine->max_postings) * sizeof(uint32_t)));
	if (!engine->stored_buffer || !engine->block_buffer || !engine->candidate_buffer) {
		Close(engine);
		return -4;
	}
	return 0;
}

void Close(Engine *engine) {
	if (!engine) return;
	if (engine->fd >= 0) sceIoClose(engine->fd);
	if (engine->stored_buffer) sce_paf_free(engine->stored_buffer);
	if (engine->block_buffer) sce_paf_free(engine->block_buffer);
	if (engine->candidate_buffer) sce_paf_free(engine->candidate_buffer);
	Reset(engine);
}

size_t NormalizeQuery(const wchar_t *input, size_t length, char *output, size_t capacity) {
	if (!output || !capacity) return 0;
	bool ascii_only = true;
	for (size_t index = 0; input && index < length; ++index) {
		uint32_t codepoint = Utf16Codepoint(input, length, &index);
		if (codepoint == 0x3000) codepoint = 0x20;
		else if (codepoint >= 0xFF01 && codepoint <= 0xFF5E) codepoint -= 0xFEE0;
		if (codepoint > 0x7F) ascii_only = false;
	}

	size_t used = 0;
	bool pending_space = false;
	for (size_t index = 0; input && index < length; ++index) {
		uint32_t codepoint = Utf16Codepoint(input, length, &index);
		if (codepoint == 0x3000) codepoint = ascii_only ? 0x20 : 0x3000;
		if (codepoint >= 0xFF01 && codepoint <= 0xFF5E) codepoint -= 0xFEE0;
		if (codepoint >= 'A' && codepoint <= 'Z') codepoint += 'a' - 'A';
		if (ascii_only) {
			if (codepoint <= 0x20) {
				pending_space = used != 0;
				continue;
			}
			if (SkipAscii(codepoint)) continue;
			if (pending_space && used + 1 < capacity) output[used++] = ' ';
			pending_space = false;
		} else {
			if (codepoint == 0x3000 || codepoint <= 0x20 || SkipAscii(codepoint)) continue;
			if (codepoint >= 0x30A1 && codepoint <= 0x30F6) codepoint -= 0x60;
			else if (codepoint >= 0x30FD && codepoint <= 0x30FE) codepoint -= 0x60;
		}
		EncodeUtf8(codepoint, output, capacity, &used);
	}
	output[used] = 0;
	return used;
}

int Search(Engine *engine, const char *query, char *markup, size_t capacity, SearchStats *stats) {
	if (!engine || engine->fd < 0 || !query || !markup || !capacity || !stats) return -1;
	sce_paf_memset(stats, 0, sizeof(*stats));
	Appender out(markup, capacity);
	size_t query_length = sce_paf_strlen(query);
	if (!query_length || query_length >= QUERY_UTF8_MAX) {
		out.Add("<font color=\"#c8d4e8\">Type a Japanese word, reading, name, or English gloss.</font>");
		return 0;
	}

	out.Add("<font color=\"#8fb7ff\">Results for </font><font color=\"#ffffff\">");
	AddEscaped(&out, query);
	out.Add("</font><br/><br/>");

	char tokens[kEnglishTokenLimit][QUERY_UTF8_MAX];
	uint32_t token_count = sce_paf_strchr(query, ' ') ? SplitEnglishTokens(query, tokens) : 0;
	bool success;
	if (token_count > 1) {
		success = SearchMultipleEnglishTokens(engine, tokens, token_count, &out, stats);
	} else if (token_count == 1) {
		success = SearchPrefix(engine, tokens[0], &out, stats);
	} else {
		success = SearchPrefix(engine, query, &out, stats);
	}
	if (!success) return -2;
	if (!stats->results) out.Add("<font color=\"#aab4c4\">No matching entries.</font>");
	if (out.Truncated()) {
		stats->truncated = true;
		out.Add("<br/><font color=\"#ffcc66\">Results truncated to protect memory.</font>");
	}
	return 0;
}

} // namespace jmdict
