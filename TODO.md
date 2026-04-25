# TODO — Features Not Yet Implemented

Items are grouped by component. Priority follows the SRS: P0 > P1 > P2.

---

## Engine

### MmapBuffer
- [ ] Store the file path at `open()` time so `remap()` can re-open after external change
- [ ] Wire `remap()` into `QFileSystemWatcher` notification in `MainWindow`

### PieceTable
- [ ] `remove(pos, len)` — split at pos and pos+len, unlink pieces in between
- [ ] `replace()` currently calls remove+insert but remove is a no-op
- [ ] Undo stack size cap (512 MB default) with status bar notification on overflow

### SparseLineIndex
- [ ] `lineToOffset()` — forward scan from checkpoint using `MmapBuffer` (needs buf ref stored at build time)
- [ ] `offsetToLine()` — forward scan from checkpoint (same issue)
- [ ] CRLF handling — currently only counts `\n`
- [ ] Lazy rebuild of invalidated checkpoints on next lookup (currently just truncates)
- [ ] Word-wrap mode: track visual lines instead of file newlines (TVP-12)

### FormatEngine
- [ ] Implement streaming beautify via CMarkup node iteration (`MNT_ELEMENT`, `MNT_TEXT`, `MNT_CDATA_SECTION`, `MNT_COMMENT`, `MNT_PROCESSING_INSTRUCTION`, `MNT_DOCUMENT_TYPE`)
- [ ] Implement minify (strip insignificant whitespace-only text nodes)
- [ ] Progress callback every ~100 ms
- [ ] Wrap entire operation as single undo step
- [ ] Mixed indentation detection and display (FMT-06)

---

## UI

### ViewportRenderer — content display (P0 blocker)
- [ ] `fetchVisibleLines()` — read actual lines from `PieceTable` via `SparseLineIndex::lineToOffset()` + `Iterator`
- [ ] Text rendering per token using `IncrementalHighlighter` results
- [ ] Cursor blink and caret drawing
- [ ] Selection range rendering (Shift + movement, click-drag)
- [ ] Matching tag-pair highlight when cursor is inside a tag name (TVP-13)
- [ ] Horizontal scroll: measure longest visible line, update h-scroll range

### ViewportRenderer — keyboard input
- [ ] Arrow keys, Home/End, Page Up/Down, Ctrl+Home/End (TVP-06)
- [ ] Shift + movement for selection (TVP-07)
- [ ] Cut / Copy / Paste / Select All (TVP-08)
- [ ] Printable character insert → `PieceTable::insert()`
- [ ] Backspace / Delete → `PieceTable::remove()`
- [ ] Tab rendering with configurable stop width (TVP-06)
- [ ] Auto-close tags on `>` (EDT-06, configurable)

### ViewportRenderer — mouse input
- [ ] Click → hit-test line/column → update `m_cursorOffset`
- [ ] Click-drag → selection
- [ ] Double-click → select word / tag name

### IncrementalHighlighter
- [ ] XML token state machine (ARC-HL-02): all 10 token kinds
- [ ] Dark theme color palette
- [ ] Per-theme configuration via preferences dialog

### VirtualTreeModel
- [ ] `fetchMore()` / `loadChildren()` — seek CMarkup to `byteOffset`, parse forward to parent depth
- [ ] Row numbering among siblings (needed for `parent()` and `index()`)
- [ ] Column 1: attribute summary `[@attr="…"]`
- [ ] Column 2: text content preview (≤ 60 chars)
- [ ] AsyncLoader phase 3: populate root-level nodes using CMarkup

### AsyncLoader
- [ ] Phase 3: use CMarkup `MDF_READFILE` to parse level-1 tree nodes and call `appendRootNodes()`

### MainWindow — sync between panes
- [ ] `onCursorMoved()` → highlight matching tree node + scroll tree view
- [ ] `onTreeNodeActivated()` → scroll viewport and update cursor (offset → viewport)
- [ ] Breadcrumb bar update on cursor move (XPath from root to current element)
- [ ] Attribute panel: populate from CMarkup at cursor position
- [ ] Attribute panel: in-place editing commits to `PieceTable`

### Search (SRC-*)
- [ ] Inline search bar (Ctrl+F) at bottom of viewport
- [ ] Find & Replace dialog (Ctrl+H)
- [ ] Plain text, case-insensitive, POSIX extended regex modes
- [ ] XPath 1.0 search (Ctrl+Shift+F) with tree + viewport highlight
- [ ] Background search thread for files ≥ 100 MB with streaming results panel
- [ ] F3 / Shift+F3 next/previous with document wrap-around
- [ ] Replace All as single undo step
- [ ] Search result highlight colors (current match vs other matches)
- [ ] SIMD-optimised `memmem` for ≥ 500 MB/s throughput (PF-07)

### Format menu
- [ ] Beautify (Ctrl+Shift+B) — run `FormatEngine` in background thread for large files
- [ ] Minify — same, background for ≥ 100 MB files
- [ ] Progress dialog with Cancel button
- [ ] Swap current `PieceTable` with formatted result as single undo step

### Navigation (NAV-*)
- [ ] Go To Line dialog (Ctrl+G) — instant O(log n) jump
- [ ] Go To Byte Offset command
- [ ] Navigation history back/forward (Alt+Left / Alt+Right)
- [ ] Bookmarks: Ctrl+B adds, bookmark panel lists, persisted per file

### Validation (VAL-*)
- [ ] Background well-formedness check via `CMarkup::IsWellFormed()`, debounced 500 ms
- [ ] Status bar green check / red X with line number
- [ ] Red squiggle gutter markers and Errors panel
- [ ] DTD validation command (optional, when DOCTYPE present)

### Encoding (ENC-*)
- [ ] Detect encoding from BOM → XML declaration → UTF-8 auto-detect
- [ ] Display current encoding in status bar
- [ ] Change encoding command (converts in-memory + updates XML declaration)
- [ ] Unicode code point / UTF-8 bytes / block name in status bar for non-ASCII cursor
- [ ] Convert Char Ref command (`&#8364;` ↔ `€`)

### Edit operations (EDT-*)
- [ ] Tag wrap: wrap selection in new element (user-supplied name)
- [ ] Tag unwrap: remove surrounding tag, keep inner content
- [ ] Auto-completion: tag names and attribute names from document vocabulary

### Session & Preferences (SES-*)
- [ ] Recent files list (≥ 20 entries) in File menu
- [ ] Reopen last file on launch
- [ ] Per-file state: scroll position, cursor, bookmarks, word-wrap
- [ ] Preferences dialog (Ctrl+,): font, indent style, color theme, column markers, auto-close, recent file count, large-file save strategy
- [ ] Light / Dark / System theme toggle
- [ ] Crash recovery: auto-save every 60 s when unsaved changes exist, offer at next launch (REL-05)

### File I/O (FIO-*)
- [ ] Atomic save: write to temp file in same directory, then `rename()` (FIO-06)
- [ ] Large-file save choice (≥ 500 MB): in-place patch vs full rewrite (FIO-07)
- [ ] `QFileSystemWatcher` for external modification detection + reload prompt (FIO-08)
- [ ] New empty XML document (FIO-10)

---

## Tests

- [ ] `tst_MmapBuffer`: all `QSKIP` cases (open, size, slice, mmap/pread paths, move semantics)
- [ ] `tst_PieceTable`: remove, replace, undo/redo, iterator, concurrent reads
- [ ] `tst_SparseLineIndex`: all cases including CRLF, latency, cancellation, invalidation
- [ ] `tst_FormatEngine`: all cases (needs CMarkup wired up)
- [ ] Regression tests: reference files 1 KB → 2 GB
- [ ] Memory assertions: RSS ≤ 80 MB for 2 GB files
- [ ] Round-trip tests: open → beautify → save → open byte-identical
- [ ] Fuzz targets: `IncrementalHighlighter` and `VirtualTreeModel` via libFuzzer
- [ ] Performance benchmarks with CI regression alerts (> 10% tolerance)
- [ ] UI smoke tests via `QTest::mouseClick` / `QTest::keyClick`

---

## Packaging & Distribution

- [ ] AppImage: integrate `linuxdeploy` + Qt plugin into CI (Linux job)
- [ ] macOS: `macdeployqt` + `create-dmg`, code-sign and notarise
- [ ] Linux `.desktop` MIME type registration (`application/xml`, `text/xml`)
- [ ] AppStream metadata for software-center discovery
- [ ] macOS `QFileOpenEvent` handling (Finder open, Dock drag)
- [ ] XKB library (`libxkbcommon-dev`) listed as build dependency in README
