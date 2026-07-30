# DONE — Implemented Features

Measured figures below come from a 2.15 GB test file on this machine (Linux,
SSD, Release build). "Element-dense" means ~216 M elements (≈10 bytes each);
"typical" means ~8.5 M records of ~250 bytes.

| Metric | Target | Measured |
|---|---|---|
| RAM, freshly opened 2.15 GB file | ≤ 80 MB | 54–71 MB peak (Qt baseline alone is 35 MB) |
| Time to first visible content | < 1 s | < 1 s (content painted while the index builds) |
| Line index build | — | 0.8–1.4 s (1.5–2.7 GB/s) |
| Tree level-1 population | < 3 s | 28 ms typical; bounded by an explicit byte cap |
| Plain-text search | ≥ 500 MB/s | uses `memmem`; ~1 GB/s on cached data |
| Beautify throughput | ≥ 50 MB/s | ~200 MB/s |

## Project scaffold
- CMake 3.24+ build system with `vcpkg.json` dependency manifest
- `loxe_engine` static library (engine with no Qt dependency) + `loxe` Qt executable
- Debug builds enable ASan + UBSan automatically; the whole suite passes clean
  under both with leak detection on
- GitHub Actions CI matrix (ubuntu-22.04 + macos-13)
- Linux `.desktop` file and macOS `Info.plist` bundle metadata
- 8 Qt Test binaries, ~140 test functions, all passing

## Engine layer

### MmapBuffer
- Opens files read-only via `mmap(PROT_READ, MAP_PRIVATE)`
- Automatic fallback to `pread()` when `mmap()` fails; the pread path uses a
  thread-local scratch buffer so concurrent readers cannot clobber each other
- `slice()` clamps to the file size and caps pread slices at 1 MB (callers loop)
- `adviseSequential()` / `adviseRandom()` / `adviseDontNeed()` via `madvise()`
- `adviseDontNeed()` is what keeps RSS flat: mapped pages count toward RSS while
  resident, so a full 2 GB pass would otherwise blow the budget on its own
- Stores the path at `open()` so `remap()` can re-open after an external change
- Rejects non-regular files; supports zero-length files
- Move semantics; non-copyable

### PieceTable
- Two-buffer model: FILE (memory-mapped) + ADD (append-only in-memory string)
- `insert(pos, text)` — splits at pos; coalesces consecutive typing into the
  preceding ADD piece so 100 keystrokes produce 1 piece, not 100
- `remove(pos, len)` — splits at the boundary, unlinks whole pieces, trims the last
- `replace()` — remove + insert wrapped as one undo step
- `replaceAll()` / `appendInitial()` for whole-document swaps and fresh documents
- Undo/redo storing both pre- and post-edit piece lists, with a 512 MB retained
  memory cap that discards oldest history and reports truncation
- Nestable `beginUndoGroup()` / `endUndoGroup()` so compound operations
  (beautify, replace-all) undo in a single step
- `readInto()` / `read()` / `chunkAt()` for bounded random access
- `releasePagesBefore()` lets sequential scanners drop file pages behind them
- Sequential `Iterator` yielding bounded chunks (≤ 1 MB), used for saving,
  formatting and search
- Reader-writer lock (`std::shared_mutex`) for one writer + multiple readers

### SparseLineIndex
- Checkpoint array of `(line, byte_offset)` pairs every ~4 KB
- Indexes the **PieceTable**, not the raw file, so lookups stay correct after edits
- Chunked `memchr` scan at 1.5–2.7 GB/s, releasing file pages as it advances
- `attach()` makes lookups usable before any scanning — this is what lets the
  viewport paint the first screenful immediately
- Bounded forward scans in `lineToOffset()` / `offsetToLine()` / `lineEndOffset()`
- `invalidateFrom()` drops stale checkpoints; later lookups extend the array
  lazily from the last valid checkpoint and never rescan from byte 0
- CRLF-safe: `\r` is treated as part of the line terminator
- `lineCount()` exact; `estimatedLineCount()` extrapolates during loading
- Background build with cancellation and progress reporting

### XmlScanner (new)
- Streaming XML tokeniser shared by FormatEngine and VirtualTreeModel
- Reports Text / StartTag / EndTag / EmptyTag / Comment / CDATA / PI / DOCTYPE
  with byte offsets, holding at most one node in memory
- Zero-copy fast path: nodes contained in one read chunk are handed out as views
  into the read buffer, with a per-byte fallback for nodes spanning a boundary
- Correct on the cases that break naive scanners: `>` inside attribute values,
  `<` inside comments and CDATA, `>` inside a DOCTYPE internal subset
- Splits oversized text nodes so a single giant text node still streams
- Releases file pages as it advances

### FormatEngine
- Streaming beautify and minify built on XmlScanner
- `formatToSink()` streams output in blocks capped at 8 MB — the document is
  never assembled in memory
- Beautify: configurable indent width and tabs-vs-spaces; text-only elements
  stay on one line; comments, CDATA, PIs and DOCTYPEs re-emitted verbatim
- Minify: strips whitespace-only text nodes, preserves significant text
- Idempotent, and beautify → minify round-trips back to the original
- Progress callbacks throttled to ~100 ms; cancellation supported

### SearchEngine (new)
- Plain and case-insensitive search over the PieceTable
- Overlapping 1 MB windows so matches straddling a window boundary are found
- Delegates to `memmem()` on glibc for SIMD throughput
- Forward, backward, count-all, optional wrap-around, cancellable

### Encoding (new)
- Detection per ENC-01: BOM → XML declaration → UTF-8 validity → Latin-1
- UTF-8 validator tolerant of sequences truncated by the probe boundary

## UI layer

### AsyncLoader
- Three-phase background load on a dedicated `QThread`:
  - Phase 1: `mmap()` + PieceTable + an *attached* index → `fileReady()`.
    The index resolves lookups lazily, so the viewport paints immediately
    instead of waiting for the full scan.
  - Phase 2: full `SparseLineIndex` scan → `indexReady()`
  - Phase 3: document element **and its level-1 children** → `treeReady()`
- Level-1 enumeration runs here, on the worker thread, on purpose: it is one
  pass over the document and must not land on the UI thread when the tree view
  first expands the root
- Hands the model's thread affinity to the main thread before publishing it
- Progress signals, cancellation via atomic flag, `wait()` on destruction

### ViewportRenderer
- Paints only the visible lines, with the gutter, current-line highlight,
  selection, search-match highlight and configurable column markers
- UTF-8 aware: each visible line carries a UTF-16-unit → byte-offset map, so
  cursor offsets stay exact for multi-byte characters
- Tab expansion on a cell grid shared by painting and hit-testing
- Blinking caret honouring the platform flash time
- Keyboard: arrows, Home/End, Ctrl+Home/End, Page Up/Down, Shift+movement for
  selection, Enter, Tab, Backspace, Delete, Ctrl+A/C/X/V
- "Sticky column" so moving down through a short line and back out restores the
  original column
- CRLF and multi-byte characters delete as single units
- Mouse: click to position, drag to select with edge auto-scroll, double-click
  to select a word
- Editing goes through PieceTable only; replacing a selection is one undo step
- Read-only mode rejects all mutations
- Vertical and horizontal scroll bars, the latter sized from the widest visible line
- Light and dark palettes

### IncrementalHighlighter
- Full XML tokeniser over viewport lines only, covering all ten token kinds
- Parser state carries across lines, so comments, CDATA sections, PIs, DOCTYPEs
  and multi-line attribute values colour correctly
- Adjacent same-kind runs merge into one token to cut `drawText` calls
- Light and dark palettes; context-window caching across identical repaints

### VirtualTreeModel
- `QAbstractItemModel` over a flat `std::vector<TreeNode>`; nodes store byte
  offsets, depth and display strings — never document content
- Reads the **live PieceTable**, so the tree matches unsaved edits
- Children discovered by streaming just that element's byte range, in one pass
  that also yields each child's `hasChildren`, end offset and text preview
- Bounded two ways: 50,000 children per expansion and 768 MB scanned per
  expansion, whichever trips first; the parent is then flagged partial
- Three columns: element, first-attribute summary, text preview
- `indexForOffset()` maps a caret position back to the deepest loaded node

### XmlContext (new)
- Recovers the open-element chain around a byte offset by scanning backwards
  with a bounded budget (4 MB), matching end tags against start tags
- Feeds the breadcrumb bar and the attribute panel

### FindBar (new)
- Inline find / replace bar: incremental search as you type, next/previous,
  match case, regular expressions, replace, replace all, Esc to dismiss

### MainWindow
- Dual-pane layout: viewport + tree in a `QSplitter`, find bar beneath the
  viewport, dockable attribute panel, breadcrumb bar
- Atomic save via `QSaveFile` (temp file + `rename()`), streaming the piece list
  so the document is never assembled in memory; Save As; dirty tracking with an
  unsaved-changes prompt on close, open and new
- New empty document
- Undo/redo with actions enabled from the PieceTable's real state
- Find (Ctrl+F), Find & Replace (Ctrl+H), F3 / Shift+F3, Replace All as a single
  undo step, regex search via `QRegularExpression` with byte↔UTF-16 conversion
- Go to Line (Ctrl+G)
- Beautify (Ctrl+Shift+B) and Minify (Ctrl+Shift+M) with a cancellable progress
  dialog, applied as one undo step
- Debounced (500 ms) well-formedness check via `CMarkup::IsWellFormed()`, with
  the error in the status bar tooltip
- Encoding detection shown in the status bar, BOM flagged
- Recent files (20 entries) persisted across sessions
- Breadcrumb and attribute panel follow the caret; caret selects the matching
  tree node; activating a tree node moves the caret
- `QFileSystemWatcher` external-modification detection with a reload prompt
- Word wrap, tree pane, attribute pane and dark theme toggles, persisted
- Session geometry, window state and splitter position via `QSettings`
- Drag-and-drop file opening

### CLI
- `--line N`, `--search TERM`, `--ro` all wired up
- `--screenshot PATH` / `--screenshot-delay MS` renders the window and exits,
  which is how the UI is verified headlessly

## third_party / CMarkup
- `Markup.cpp` / `Markup.h` compiled as `libcmarkup.a`
- `MARKUP_ICONV` enabled on POSIX; `iconv` linked only on macOS
- Used for well-formedness validation only. **The vendored CMarkup 11.5 exposes
  no streaming file API** — `Load()` explicitly fails when `MDF_READFILE` is set,
  and there is no public `Open()` — so the tree cannot be built by seeking
  CMarkup to a byte offset as originally designed. `SetDoc()` would require the
  whole document in memory, which the 80 MB budget rules out. `XmlScanner`
  exists for that reason; see [TODO.md](TODO.md).
