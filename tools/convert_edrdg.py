#!/usr/bin/env python3
"""Stream EDRDG XML into the compact, disk-seekable JMdict Vita format.

The Vita never parses XML or maps the index. XML entry state is bounded,
global key ordering is delegated to disk-backed SQLite, index keys are
front-coded, and entries share independently decompressible 64 KiB blocks.
"""

from __future__ import annotations

import argparse
import gzip
import html
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import sys
import xml.etree.ElementTree as ET
import zlib


MAGIC = b"JMDVITA1"
VERSION = 3
HEADER = struct.Struct("<8s16I5Q")
KEY_BLOCK_RECORD = struct.Struct("<I")
ENTRY_BLOCK_RECORD = struct.Struct("<IIIII")
ENTRY_LENGTH = struct.Struct("<H")
MAX_KEY_BYTES = 511
MAX_ENTRY_BYTES = 48 * 1024
ENTRY_BLOCK_TARGET = 64 * 1024
KEYS_PER_BLOCK = 64
COPY_BUFFER = 1024 * 1024
XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"

SOURCE_JMDICT = 0
SOURCE_JMNEDICT = 1

RANK_PRIMARY = 0
RANK_WRITTEN = 1
RANK_READING = 2
RANK_TOKEN = 3

ASCII_PUNCTUATION = (
    set(range(0x21, 0x30))
    | set(range(0x3A, 0x41))
    | set(range(0x5B, 0x61))
    | set(range(0x7B, 0x7F))
)
ENGLISH_TOKEN = re.compile(r"[A-Za-z][A-Za-z0-9+'-]{1,}")
STOP_WORDS = {
    "a", "an", "and", "as", "at", "be", "by", "for", "from", "in",
    "is", "of", "on", "one", "or", "someone", "something", "the", "to", "with",
}


def normalize_key(value: str) -> str:
    """Mirror the byte-key normalizer implemented on Vita."""
    output: list[str] = []
    for character in value:
        codepoint = ord(character)
        if codepoint == 0x3000:
            continue
        if 0xFF01 <= codepoint <= 0xFF5E:
            codepoint -= 0xFEE0
        if 0x41 <= codepoint <= 0x5A:
            codepoint += 0x20
        if codepoint <= 0x20 or codepoint in ASCII_PUNCTUATION:
            continue
        if 0x30A1 <= codepoint <= 0x30F6 or 0x30FD <= codepoint <= 0x30FE:
            codepoint -= 0x60
        output.append(chr(codepoint))
    return "".join(output)


def normalize_query(value: str) -> str:
    """Preserve word boundaries only for all-ASCII English queries."""
    mapped: list[int] = []
    ascii_only = True
    for character in value:
        codepoint = ord(character)
        if codepoint == 0x3000:
            codepoint = 0x20
        elif 0xFF01 <= codepoint <= 0xFF5E:
            codepoint -= 0xFEE0
        mapped.append(codepoint)
        if codepoint > 0x7F:
            ascii_only = False
    if not ascii_only:
        return normalize_key(value)

    output: list[str] = []
    pending_space = False
    for codepoint in mapped:
        if codepoint <= 0x20:
            pending_space = bool(output)
            continue
        if 0x41 <= codepoint <= 0x5A:
            codepoint += 0x20
        if codepoint in ASCII_PUNCTUATION:
            continue
        if pending_space and output:
            output.append(" ")
        pending_space = False
        output.append(chr(codepoint))
    return "".join(output)


def _clean(value: str | None) -> str:
    return " ".join((value or "").split())


def _texts(parent: ET.Element, tag: str) -> tuple[str, ...]:
    return tuple(filter(None, (_clean(child.text) for child in parent.findall(tag))))


def _english_glosses(sense: ET.Element) -> tuple[str, ...]:
    values: list[str] = []
    for child in sense.findall("gloss"):
        language = child.attrib.get(XML_LANG, "eng")
        value = _clean(child.text)
        if language in ("", "eng") and value:
            values.append(value)
    return tuple(values)


def _unique(values: list[str] | tuple[str, ...]) -> tuple[str, ...]:
    return tuple(dict.fromkeys(value for value in values if value))


def _escaped(value: str) -> str:
    return html.escape(value, quote=True)


def _line(label: str, values: tuple[str, ...], color: str = "#a9bad2") -> str:
    if not values:
        return ""
    return f'<font color="{color}">{label}</font> {_escaped("、".join(values))}<br/>'


def _bounded_markup(fragments: list[str]) -> bytes:
    selected: list[bytes] = []
    used = 0
    reserve = 128
    for fragment in fragments:
        encoded = fragment.encode("utf-8")
        if used + len(encoded) + reserve > MAX_ENTRY_BYTES:
            selected.append(b'<font color="#ffcc66">Entry truncated for Vita display.</font><br/>')
            break
        selected.append(encoded)
        used += len(encoded)
    result = b"".join(selected)
    if not result or len(result) > MAX_ENTRY_BYTES or len(result) > 0xFFFF:
        raise ValueError("bounded markup construction failed")
    return result


def _forms(entry: ET.Element) -> tuple[tuple[str, ...], tuple[str, ...]]:
    written = _unique([_clean(node.findtext("keb")) for node in entry.findall("k_ele")])
    readings = _unique([_clean(node.findtext("reb")) for node in entry.findall("r_ele")])
    return written, readings


def _form_detail_fragments(entry: ET.Element) -> list[str]:
    fragments: list[str] = []
    fragments.append(_line("Writing info", _unique([
        value for node in entry.findall("k_ele") for value in _texts(node, "ke_inf")
    ])))
    fragments.append(_line("Writing priority", _unique([
        value for node in entry.findall("k_ele") for value in _texts(node, "ke_pri")
    ])))
    fragments.append(_line("Reading info", _unique([
        value for node in entry.findall("r_ele") for value in _texts(node, "re_inf")
    ])))
    fragments.append(_line("Reading priority", _unique([
        value for node in entry.findall("r_ele") for value in _texts(node, "re_pri")
    ])))
    for node in entry.findall("r_ele"):
        reading = _clean(node.findtext("reb"))
        restrictions = _texts(node, "re_restr")
        if restrictions:
            fragments.append(_line(f"{reading} applies to", restrictions))
        if node.find("re_nokanji") is not None:
            fragments.append(_line("Reading restriction", (f"{reading}: no kanji form",)))
    return fragments


def _english_keys(values: tuple[str, ...]) -> list[tuple[str, int]]:
    """Index English words, not entire gloss phrases.

    Multiword queries are intersected on the Vita. This removes more than a
    million mostly-unique phrase keys without losing reverse-English lookup.
    """
    keys: list[tuple[str, int]] = []
    for value in values:
        for match in ENGLISH_TOKEN.findall(value):
            token = match.strip("-'").casefold()
            if len(token) >= 2 and token not in STOP_WORDS:
                keys.append((token, RANK_TOKEN))
    return keys


def parse_jmdict_entry(entry: ET.Element) -> tuple[bytes, list[tuple[str, int]]]:
    written, readings = _forms(entry)
    sequence = _clean(entry.findtext("ent_seq"))
    headword = written[0] if written else readings[0] if readings else sequence
    fragments = [
        f'<h1><font color="#ffffff">{_escaped(headword)}</font></h1>',
        f'<font color="#6fa8ff">JMdict · entry {_escaped(sequence)}</font><br/>',
        _line("Written", written),
        _line("Reading", readings, "#8fd5c4"),
        *_form_detail_fragments(entry),
        "<br/>",
    ]
    gloss_values: list[str] = []
    display_number = 0
    for sense in entry.findall("sense"):
        glosses = _english_glosses(sense)
        if not glosses:
            continue
        display_number += 1
        gloss_values.extend(glosses)
        fragments.append(
            f'<font color="#ffd27f"><b>{display_number}.</b></font> '
            f'{_escaped("; ".join(glosses))}<br/>'
        )
        fragments.append(_line("Restricted written", _texts(sense, "stagk")))
        fragments.append(_line("Restricted reading", _texts(sense, "stagr")))
        fragments.append(_line("Part of speech", _texts(sense, "pos")))
        fragments.append(_line("Field", _texts(sense, "field")))
        fragments.append(_line("Misc", _texts(sense, "misc")))
        fragments.append(_line("Dialect", _texts(sense, "dial")))
        fragments.append(_line("Notes", _texts(sense, "s_inf")))
        fragments.append(_line("Source language", _texts(sense, "lsource")))
        fragments.append(_line("See also", _texts(sense, "xref")))
        fragments.append(_line("Antonym", _texts(sense, "ant")))
        fragments.append("<br/>")

    keys: list[tuple[str, int]] = []
    for index, value in enumerate(written):
        keys.append((value, RANK_PRIMARY if index == 0 else RANK_WRITTEN))
    for index, value in enumerate(readings):
        keys.append((value, RANK_READING if written or index else RANK_PRIMARY))
    keys.extend(_english_keys(tuple(gloss_values)))
    return _bounded_markup(fragments), keys


def parse_jmnedict_entry(entry: ET.Element) -> tuple[bytes, list[tuple[str, int]]]:
    written, readings = _forms(entry)
    sequence = _clean(entry.findtext("ent_seq"))
    headword = written[0] if written else readings[0] if readings else sequence
    fragments = [
        f'<h1><font color="#ffffff">{_escaped(headword)}</font></h1>',
        f'<font color="#c89cff">JMnedict · entry {_escaped(sequence)} · proper name</font><br/>',
        _line("Written", written),
        _line("Reading", readings, "#8fd5c4"),
        *_form_detail_fragments(entry),
        "<br/>",
    ]
    translations: list[str] = []
    for number, trans in enumerate(entry.findall("trans"), 1):
        details = _texts(trans, "trans_det")
        translations.extend(details)
        fragments.append(
            f'<font color="#ffd27f"><b>{number}.</b></font> '
            f'{_escaped("; ".join(details))}<br/>'
        )
        fragments.append(_line("Name type", _texts(trans, "name_type")))
        fragments.append(_line("See also", _texts(trans, "xref")))
        fragments.append("<br/>")

    keys: list[tuple[str, int]] = []
    for index, value in enumerate(written):
        keys.append((value, RANK_PRIMARY if index == 0 else RANK_WRITTEN))
    for index, value in enumerate(readings):
        keys.append((value, RANK_READING if written or index else RANK_PRIMARY))
    keys.extend(_english_keys(tuple(translations)))
    return _bounded_markup(fragments), keys


def _open_xml(path: Path):
    return gzip.open(path, "rb") if path.suffix.casefold() == ".gz" else path.open("rb")


def iter_entries(path: Path):
    with _open_xml(path) as source:
        root: ET.Element | None = None
        for event, element in ET.iterparse(source, events=("start", "end")):
            if root is None and event == "start":
                root = element
            if event != "end" or element.tag != "entry":
                continue
            yield element
            element.clear()
            if root is not None:
                root.clear()


def _configure_database(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA journal_mode=OFF")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=FILE")
    connection.execute("PRAGMA cache_size=-32768")
    connection.execute("PRAGMA locking_mode=EXCLUSIVE")
    connection.execute(
        "CREATE TABLE keys ("
        "key BLOB NOT NULL, source INTEGER NOT NULL, rank INTEGER NOT NULL, entry_id INTEGER NOT NULL, "
        "PRIMARY KEY (key, source, rank, entry_id)) WITHOUT ROWID"
    )
    return connection


def _insert_entry_keys(
    connection: sqlite3.Connection,
    keys: list[tuple[str, int]],
    source: int,
    entry_id: int,
) -> None:
    best: dict[bytes, int] = {}
    for value, rank in keys:
        normalized = normalize_key(value)
        encoded = normalized.encode("utf-8")
        if not encoded or len(encoded) > MAX_KEY_BYTES:
            continue
        previous = best.get(encoded)
        if previous is None or rank < previous:
            best[encoded] = rank
    connection.executemany(
        "INSERT OR IGNORE INTO keys(key, source, rank, entry_id) VALUES (?, ?, ?, ?)",
        ((key, source, rank, entry_id) for key, rank in best.items()),
    )


def _copy_file(source: Path, destination) -> None:
    with source.open("rb") as handle:
        shutil.copyfileobj(handle, destination, COPY_BUFFER)


def _encode_uleb(value: int) -> bytes:
    if value < 0 or value > 0xFFFFFFFF:
        raise ValueError("ULEB128 value is outside the 32-bit format")
    output = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            output.append(byte | 0x80)
        else:
            output.append(byte)
            return bytes(output)


def _common_prefix(left: bytes, right: bytes) -> int:
    limit = min(len(left), len(right))
    index = 0
    while index < limit and left[index] == right[index]:
        index += 1
    return index


def build_dictionary(
    output: Path,
    jmdict: Path | None,
    jmnedict: Path | None,
    *,
    limit: int | None = None,
    progress_every: int = 10_000,
) -> dict[str, int]:
    if not jmdict and not jmnedict:
        raise ValueError("at least one EDRDG input is required")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    stem = output.name + ".work"
    database_path = output.parent / f"{stem}.sqlite3"
    key_blocks_path = output.parent / f"{stem}.key-blocks"
    index_stream_path = output.parent / f"{stem}.index-stream"
    entry_blocks_path = output.parent / f"{stem}.entry-blocks"
    entry_data_path = output.parent / f"{stem}.entry-data"
    final_path = output.parent / f"{stem}.final"
    temporary_paths = (
        database_path,
        key_blocks_path,
        index_stream_path,
        entry_blocks_path,
        entry_data_path,
        final_path,
    )
    for path in temporary_paths:
        path.unlink(missing_ok=True)

    connection = _configure_database(database_path)
    counts = [0, 0]
    entry_count = 0
    entry_block_count = 0
    max_entry_size = 0
    max_block_raw = 0
    max_block_stored = 0
    raw_block = bytearray()
    block_first_entry = 0
    block_entry_count = 0

    try:
        with entry_blocks_path.open("wb") as entry_blocks, entry_data_path.open("wb") as entry_data:
            def flush_entry_block() -> None:
                nonlocal entry_block_count, max_block_raw, max_block_stored
                nonlocal raw_block, block_first_entry, block_entry_count
                if not raw_block:
                    return
                compressed = zlib.compress(raw_block, level=9)
                stored = compressed if len(compressed) < len(raw_block) else bytes(raw_block)
                data_offset = entry_data.tell()
                if data_offset > 0xFFFFFFFF:
                    raise ValueError("entry data exceeds the 32-bit Vita format")
                entry_blocks.write(ENTRY_BLOCK_RECORD.pack(
                    block_first_entry,
                    data_offset,
                    len(stored),
                    len(raw_block),
                    block_entry_count,
                ))
                entry_data.write(stored)
                entry_block_count += 1
                max_block_raw = max(max_block_raw, len(raw_block))
                max_block_stored = max(max_block_stored, len(stored))
                raw_block.clear()
                block_entry_count = 0

            inputs = (
                (jmdict, SOURCE_JMDICT, parse_jmdict_entry, "JMdict"),
                (jmnedict, SOURCE_JMNEDICT, parse_jmnedict_entry, "JMnedict"),
            )
            for path, source_id, parser, label in inputs:
                if path is None:
                    continue
                for entry in iter_entries(path):
                    markup, search_keys = parser(entry)
                    encoded_size = ENTRY_LENGTH.size + len(markup)
                    if raw_block and len(raw_block) + encoded_size > ENTRY_BLOCK_TARGET:
                        flush_entry_block()
                    if not raw_block:
                        block_first_entry = entry_count
                    raw_block.extend(ENTRY_LENGTH.pack(len(markup)))
                    raw_block.extend(markup)
                    block_entry_count += 1
                    _insert_entry_keys(connection, search_keys, source_id, entry_count)
                    counts[source_id] += 1
                    entry_count += 1
                    max_entry_size = max(max_entry_size, len(markup))
                    if progress_every and counts[source_id] % progress_every == 0:
                        print(f"{label}: {counts[source_id]:,} entries", file=sys.stderr, flush=True)
                        connection.commit()
                    if limit and counts[source_id] >= limit:
                        break
                connection.commit()
            flush_entry_block()

        association_count = connection.execute("SELECT count(*) FROM keys").fetchone()[0]
        print(f"grouping/writing {association_count:,} index associations", file=sys.stderr, flush=True)
        key_count = 0
        key_block_count = 0
        unique_key_bytes = 0
        max_postings = 0
        with key_blocks_path.open("wb") as key_blocks, index_stream_path.open("wb") as index_stream:
            cursor = connection.execute(
                # Entry IDs are assigned to JMdict first and JMnedict second,
                # so this both preserves source priority and makes every
                # posting list strictly increasing for in-place intersection.
                "SELECT key, entry_id FROM keys ORDER BY key, entry_id"
            )
            row = cursor.fetchone()
            previous_key = b""
            while row is not None:
                key = bytes(row[0])
                postings: list[int] = []
                while row is not None and bytes(row[0]) == key:
                    entry_id = int(row[1])
                    if not postings or postings[-1] != entry_id:
                        postings.append(entry_id)
                    row = cursor.fetchone()

                if key_count % KEYS_PER_BLOCK == 0:
                    stream_offset = index_stream.tell()
                    if stream_offset > 0xFFFFFFFF:
                        raise ValueError("index stream exceeds the 32-bit Vita format")
                    key_blocks.write(KEY_BLOCK_RECORD.pack(stream_offset))
                    key_block_count += 1
                    previous_key = b""
                prefix = _common_prefix(previous_key, key)
                suffix = key[prefix:]
                index_stream.write(_encode_uleb(prefix))
                index_stream.write(_encode_uleb(len(suffix)))
                index_stream.write(_encode_uleb(len(postings)))
                index_stream.write(suffix)
                for entry_id in postings:
                    index_stream.write(_encode_uleb(entry_id))
                previous_key = key
                key_count += 1
                unique_key_bytes += len(key)
                max_postings = max(max_postings, len(postings))

        index_blocks_offset = HEADER.size
        index_stream_offset = index_blocks_offset + key_blocks_path.stat().st_size
        entry_blocks_offset = index_stream_offset + index_stream_path.stat().st_size
        entry_data_offset = entry_blocks_offset + entry_blocks_path.stat().st_size
        file_size = entry_data_offset + entry_data_path.stat().st_size
        if file_size > 0xFFFFFFFF:
            raise ValueError("dictionary exceeds the 32-bit Vita format")
        flags = (1 if jmdict else 0) | (2 if jmnedict else 0)
        header = HEADER.pack(
            MAGIC,
            VERSION,
            HEADER.size,
            flags,
            entry_count,
            counts[SOURCE_JMDICT],
            counts[SOURCE_JMNEDICT],
            key_count,
            key_block_count,
            entry_block_count,
            KEYS_PER_BLOCK,
            MAX_KEY_BYTES,
            max_postings,
            max_entry_size,
            max_block_raw,
            max_block_stored,
            0,
            index_blocks_offset,
            index_stream_offset,
            entry_blocks_offset,
            entry_data_offset,
            file_size,
        )
        with final_path.open("wb") as final_file:
            final_file.write(header)
            _copy_file(key_blocks_path, final_file)
            _copy_file(index_stream_path, final_file)
            _copy_file(entry_blocks_path, final_file)
            _copy_file(entry_data_path, final_file)
            final_file.flush()
            os.fsync(final_file.fileno())
        if final_path.stat().st_size != file_size:
            raise ValueError("final dictionary size mismatch")
        os.replace(final_path, output)
        return {
            "jmdict_entries": counts[SOURCE_JMDICT],
            "jmnedict_entries": counts[SOURCE_JMNEDICT],
            "index_associations": association_count,
            "unique_keys": key_count,
            "unique_key_bytes": unique_key_bytes,
            "key_blocks": key_block_count,
            "entry_blocks": entry_block_count,
            "max_postings": max_postings,
            "max_entry_bytes": max_entry_size,
            "max_block_raw": max_block_raw,
            "max_block_stored": max_block_stored,
            "file_bytes": file_size,
        }
    finally:
        connection.close()
        for path in temporary_paths:
            path.unlink(missing_ok=True)


class DictionaryReader:
    """Host verifier that mirrors the Vita's seek/read and block-cache lookup."""

    def __init__(self, path: Path):
        self.path = path
        self.handle = path.open("rb")
        values = HEADER.unpack(self.handle.read(HEADER.size))
        (
            magic,
            version,
            header_size,
            self.flags,
            self.entry_count,
            self.jmdict_count,
            self.jmnedict_count,
            self.key_count,
            self.key_block_count,
            self.entry_block_count,
            self.keys_per_block,
            self.max_key_bytes,
            self.max_postings,
            self.max_entry_size,
            self.max_block_raw,
            self.max_block_stored,
            _reserved,
            self.index_blocks_offset,
            self.index_stream_offset,
            self.entry_blocks_offset,
            self.entry_data_offset,
            self.file_size,
        ) = values
        if magic != MAGIC or version != VERSION or header_size != HEADER.size:
            raise ValueError("unsupported dictionary header")
        if self.keys_per_block != KEYS_PER_BLOCK or path.stat().st_size != self.file_size:
            raise ValueError("dictionary layout does not match header")
        self._cached_block = -1
        self._cached_first_entry = 0
        self._cached_entry_count = 0
        self._cached_data = b""

    def close(self) -> None:
        self.handle.close()

    def _read_uleb(self) -> int:
        value = 0
        shift = 0
        for _ in range(5):
            encoded = self.handle.read(1)
            if len(encoded) != 1:
                raise ValueError("truncated index ULEB128")
            byte = encoded[0]
            value |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return value
            shift += 7
        raise ValueError("invalid index ULEB128")

    def _key_header(self, previous: bytes) -> tuple[bytes, int]:
        prefix = self._read_uleb()
        suffix_length = self._read_uleb()
        postings = self._read_uleb()
        if prefix > len(previous) or prefix + suffix_length > self.max_key_bytes:
            raise ValueError("invalid front-coded key")
        suffix = self.handle.read(suffix_length)
        if len(suffix) != suffix_length:
            raise ValueError("truncated front-coded key")
        return previous[:prefix] + suffix, postings

    def _read_postings(self, count: int) -> list[int]:
        result = [self._read_uleb() for _ in range(count)]
        if any(entry_id >= self.entry_count for entry_id in result):
            raise ValueError("invalid entry ID in postings")
        return result

    def _key_block_offset(self, block: int) -> int:
        self.handle.seek(self.index_blocks_offset + block * KEY_BLOCK_RECORD.size)
        data = self.handle.read(KEY_BLOCK_RECORD.size)
        if len(data) != KEY_BLOCK_RECORD.size:
            raise ValueError("truncated key block table")
        return KEY_BLOCK_RECORD.unpack(data)[0]

    def _first_key(self, block: int) -> bytes:
        self.handle.seek(self.index_stream_offset + self._key_block_offset(block))
        key, _postings = self._key_header(b"")
        return key

    def _start_block(self, query: bytes) -> int:
        low, high = 0, self.key_block_count
        while low < high:
            middle = low + (high - low) // 2
            if self._first_key(middle) <= query:
                low = middle + 1
            else:
                high = middle
        return 0 if low == 0 else low - 1

    def _keys_from_block(self, block: int):
        key_index = block * self.keys_per_block
        self.handle.seek(self.index_stream_offset + self._key_block_offset(block))
        previous = b""
        while key_index < self.key_count:
            if key_index % self.keys_per_block == 0:
                previous = b""
            key, posting_count = self._key_header(previous)
            postings = self._read_postings(posting_count)
            yield key, postings
            previous = key
            key_index += 1

    def _exact_postings(self, query: bytes) -> list[int]:
        for key, postings in self._keys_from_block(self._start_block(query)):
            if key == query:
                return postings
            if key > query:
                break
        return []

    def _entry_block_record(self, block: int) -> tuple[int, int, int, int, int]:
        self.handle.seek(self.entry_blocks_offset + block * ENTRY_BLOCK_RECORD.size)
        data = self.handle.read(ENTRY_BLOCK_RECORD.size)
        if len(data) != ENTRY_BLOCK_RECORD.size:
            raise ValueError("truncated entry block table")
        return ENTRY_BLOCK_RECORD.unpack(data)

    def _load_entry_block(self, block: int) -> None:
        first, data_offset, stored_length, raw_length, count = self._entry_block_record(block)
        self.handle.seek(self.entry_data_offset + data_offset)
        stored = self.handle.read(stored_length)
        if len(stored) != stored_length:
            raise ValueError("truncated entry block")
        raw = stored if stored_length == raw_length else zlib.decompress(stored)
        if len(raw) != raw_length:
            raise ValueError("entry block length mismatch")
        self._cached_block = block
        self._cached_first_entry = first
        self._cached_entry_count = count
        self._cached_data = raw

    def _entry(self, entry_id: int) -> str:
        low, high = 0, self.entry_block_count
        while low < high:
            middle = low + (high - low) // 2
            first = self._entry_block_record(middle)[0]
            if first <= entry_id:
                low = middle + 1
            else:
                high = middle
        block = 0 if low == 0 else low - 1
        if block != self._cached_block:
            self._load_entry_block(block)
        relative = entry_id - self._cached_first_entry
        if relative < 0 or relative >= self._cached_entry_count:
            raise ValueError("entry ID outside block")
        offset = 0
        for _ in range(relative):
            length = ENTRY_LENGTH.unpack_from(self._cached_data, offset)[0]
            offset += ENTRY_LENGTH.size + length
        length = ENTRY_LENGTH.unpack_from(self._cached_data, offset)[0]
        start = offset + ENTRY_LENGTH.size
        return self._cached_data[start:start + length].decode("utf-8")

    def search(self, value: str, limit: int = 20) -> list[str]:
        normalized = normalize_query(value)
        if not normalized:
            return []
        tokens = [token for token in normalized.split(" ") if token and token not in STOP_WORDS]
        if len(tokens) > 1:
            posting_lists = [self._exact_postings(token.encode("utf-8")) for token in dict.fromkeys(tokens[:4])]
            if not posting_lists or any(not postings for postings in posting_lists):
                return []
            entry_ids = sorted(set(posting_lists[0]).intersection(*map(set, posting_lists[1:])))[:limit]
        else:
            query = normalized.encode("utf-8")
            entry_ids = []
            used: set[int] = set()
            for key, postings in self._keys_from_block(self._start_block(query)):
                if not key.startswith(query):
                    if key > query:
                        break
                    continue
                for entry_id in postings:
                    if entry_id not in used:
                        used.add(entry_id)
                        entry_ids.append(entry_id)
                        if len(entry_ids) >= limit:
                            break
                if len(entry_ids) >= limit:
                    break
        return [self._entry(entry_id) for entry_id in entry_ids]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jmdict", type=Path, help="JMdict English XML or XML.gz")
    parser.add_argument("--jmnedict", type=Path, help="JMnedict XML or XML.gz")
    parser.add_argument("--output", type=Path, required=True, help="output .bin path")
    parser.add_argument("--limit", type=int, help="development limit per input")
    parser.add_argument("--progress-every", type=int, default=10_000)
    parser.add_argument("--verify", action="append", default=[], metavar="QUERY")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    report = build_dictionary(
        args.output,
        args.jmdict,
        args.jmnedict,
        limit=args.limit,
        progress_every=args.progress_every,
    )
    print(" ".join(f"{key}={value}" for key, value in report.items()))
    if args.verify:
        reader = DictionaryReader(args.output)
        try:
            for query in args.verify:
                results = reader.search(query, 3)
                print(f"verify {query!r}: {len(results)} result(s)")
                for result in results:
                    print(re.sub(r"<[^>]+>", " ", result)[:180])
        finally:
            reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
