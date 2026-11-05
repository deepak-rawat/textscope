# Core engine

See the [developer guide](../../docs/DEVELOPER_GUIDE.md) for supported builds and
the [roadmap](../../docs/ROADMAP.md) for unimplemented optimizations.

Dependencies: C++20, Qt 6 Core/Gui/Core5Compat, and **PCRE2-16** (`libpcre2-16`
via pkg-config). Tests additionally need Qt Test. The regex implementation uses
the public native PCRE2 API, not Qt private regex internals.

## Storage and line semantics

Normal opens capture immutable bytes in RAM or a private, auto-removing
`QTemporaryFile`. Removing or editing the source afterward does not affect them.
Legacy encodings are decoded incrementally with Core5Compat and normalized to
UTF-8 in immutable storage; `encoding()` still reports the source codec and
`byteCount()` the original byte count. BOMs override the requested encoding.
UTF-8/16/32 sources retain their encoded bytes. Incomplete final code units are
replaced with U+FFFD; other decoding uses Qt's replacement policy.

Explicit `direct` opens retain an ordinary buffered file handle and detect
changes using source identity, size, and modification/change timestamps
(native Windows file information or Unix device/inode and nanosecond timestamps),
before and after reads/scans. Deletion and path replacement invalidate them.
They are **not immutable snapshots** and cannot prevent concurrent writers;
changed reads throw. Stateful encodings require immutable mode.

Temporary storage is created in the current working directory, which must be
writable when spill is necessary. Files have QTemporaryFile's private creation
semantics, remain open while in use, and are removed with the owning snapshot/set.
There are no mappings of mutable source files.

LF, CRLF, and CR terminate lines, including across chunk boundaries and in
UTF-16/32. Empty files have zero lines; a final terminator does not create a
phantom extra line. Consecutive terminators create empty lines. IDs are zero-based
64-bit values. Each line index entry occupies 16 bytes and contains only offsets,
never text. Index records spill within their share of the snapshot budget.

`line(id, maxCharacters)` bounded-reads and decodes only the requested prefix;
the limit counts Qt UTF-16 code units. Zero/negative limits mean unlimited.
`forEachLine` scans storage sequentially in 256 KiB chunks and passes full lines.
Analysis therefore requires transient memory proportional to the longest line,
in addition to chunk/decoder/regex buffers; rendering truncation never truncates
analysis. `residentBytes()` estimates owned container/cache storage, not allocator,
Qt file buffers, or process RSS.

## Sets, matching, and cache

LineSet is an immutable sorted RLE range set. Fixed-width range records contain
64-bit endpoints and prefix counts; spill uses buffered block writes and a
bounded single read-page cache, with no per-page metadata growing with file size.
Builders accept sorted ranges, coalesce overlaps/adjacency, and reject descending
input and count overflow. A single set can represent up to UINT64_MAX members.
`at` throws on invalid ranks; `next` is strictly before/after the supplied ID and
can wrap, including back to that ID if it is the only member.

All uncached text predicates (including disabled ones) share one chunk-oriented
scan. Identical predicates share results. ASCII-insensitive literals use a
precomputed skip table; when all uncached predicates are ASCII literals, UTF-8
storage is scanned as byte-line views without constructing per-line QStrings.
Non-ASCII lines requiring case folding fall back to Qt Unicode comparison.
`hasUtf8Storage()` and `forEachUtf8Line()` expose this storage scan; byte views
are valid only during the callback and contain raw stored bytes without terminators.
The optional `foldAscii` argument lowercases only ASCII letters in chunk buffers,
allowing multiple insensitive literals to share one fold pass and fast byte search.
Unicode literals use Qt's case comparison. Marker
predicates use the supplied eight sets without scanning. Enabled includes OR
together; enabled exclusions are applied afterward. No enabled includes means
the universe. Composition merges sorted ranges and subtracts exclusion ranges
rather than visiting individual lines. Single in-bounds include sets with no
exclusions share the original immutable set instead of rebuilding it.
Composition streams fixed 4,096-range read/write blocks, avoiding a lock, seek,
or container append for every range. `readRanges` can return a partial block;
callers advance by its return count. `addRanges` preserves builder ordering and
coalescing. These block buffers are bounded, independent of file size.
Worst-case fragmented sets still require time and temporary space proportional
to their range count: an 85-million-range spilled set is not an instant toggle.

Cache vectors are indexed by filter position; the caller must associate them
with the same snapshot and `predicateKey`. Keys omit ID, order, description,
enabled/excluding state, colors, and unknown attributes. Marker caches are ignored
because live marker sets are authoritative.

## Regex and failures

The supported dialect is **native PCRE2, not .NET**. No .NET helper is installed:
.NET balancing groups, character-class subtraction, and dialect-specific escape
and capture behavior are not translated. PCRE2 Unicode and UTF mode are enabled;
malformed UTF-16 subjects and regex syntax/engine errors throw rather than becoming
nonmatches. Unicode regex case folding is PCRE2's, whereas literal comparison is
Qt's. JIT is used when supported; unsupported JIT patterns use the interpreter.
Automatic callouts check cancellation during matching. Matching has a 1,000,000
operation limit, 1,000 interpreter depth limit, 8 MiB interpreter heap limit,
and up to 1 MiB JIT stack per compiled predicate. Limit exhaustion throws.

Cancellation and failures use `std::runtime_error`; cancellation's exact message
is `Cancelled`. Progress reports logical source bytes, including for normalized
legacy storage. `Analysis::scannedBytes` is zero on full cache hits and equals
the original source byte count on a scan. Callbacks execute synchronously on the
calling thread and their exceptions propagate.
