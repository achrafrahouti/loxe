# DONE — Implemented Features

## Project scaffold
- CMake 3.24+ build system with `vcpkg.json` dependency manifest
- `loxe_engine` static library (engine with no Qt dependency) + `loxe` Qt executable
- Debug builds enable ASan + UBSan automatically
- GitHub Actions CI matrix (ubuntu-22.04 + macos-13)
- Linux `.desktop` file and macOS `Info.plist` bundle metadata
- Qt Test suite skeleton with four test binaries

## Engine layer

### MmapBuffer
- Opens files read-only via `mmap(PROT_READ, MAP_PRIVATE)`
- Automatic fallback to `pread()` when `mmap()` returns `MAP_FAILED`
- `adviseSequential()` / `adviseRandom()` via `madvise()`
- Zero-copy `slice(offset, len)` returning `std::string_view`
- Move semantics; non-copyable

### PieceTable
- Two-buffer model: FILE (memory-mapped) + ADD (append-only in-memory string)
- `insert(pos, text)` — splits existing piece at pos, inserts ADD piece
- `length()` — total document byte count
- Undo stack with `undo()` / `redo()` and `canUndo()` / `canRedo()`
- Sequential `Iterator` for zero-copy chunk-by-chunk reading
- Reader-writer lock (`std::shared_mutex`) for one writer + multiple readers

### SparseLineIndex
- Checkpoint array: `(line, byte_offset)` pairs every ~4 KB
- Background-thread build with cancellation support
- `invalidateFrom(offset)` — drops stale checkpoints after an edit
- `estimatedLineCount()` — usable before build completes (for scroll bar)
- `isComplete()` flag

### FormatEngine
- Interface defined (Options: Beautify/Minify, IndentStyle, indentWidth)
- Streaming design scaffolded (reads via PieceTable iterator, 8 MB output buffer limit)

## UI layer

### AsyncLoader
- Three-phase background load on a dedicated `QThread`:
  - Phase 1: `MmapBuffer::open()` → emits `fileReady()`
  - Phase 2: `SparseLineIndex::build()` → emits `indexReady()`
  - Phase 3: `VirtualTreeModel` stub → emits `treeReady()`
- Progress signals (`loadProgress(percent, phase)`) at each phase
- Cancellation via atomic flag, checked between phases
- `cancel()` + `QThread::wait()` on destructor and re-open

### MainWindow
- Dual-pane layout: `ViewportRenderer` (left) + `QTreeView` (right) in `QSplitter`
- Dockable attribute panel (`QTableWidget`, two columns: name / value)
- Breadcrumb label at top
- Status bar: line/column, encoding, well-formedness placeholder
- Progress bar shown during load, hidden on completion
- Full menu bar: File, Edit, Format, View with Qt6-style shortcuts
- File open via dialog (Ctrl+O) and drag-and-drop onto window
- Session geometry + splitter state saved/restored via `QSettings`
- Window title updates to filename on open
- `openFile(path)` slot wired to CLI positional argument

### ViewportRenderer
- Custom `QWidget` with vertical + horizontal `QScrollBar`
- Gutter rendering (line numbers, right-aligned)
- Current-line background highlight
- Column marker lines (col 80 and 120, configurable)
- `cursorMoved(uint64_t)` signal emitted on cursor change
- `setWordWrap()`, `setTabWidth()`, `setColumnMarkers()` configuration

### IncrementalHighlighter
- Ten token kinds: TagName, AttrName, AttrValue, Text, Comment, CdataSection,
  ProcessingInstruction, Doctype, EntityRef, Punctuation
- Default light-theme color palette
- `setTheme()` for color theme replacement
- Context-window caching: skips re-scanning context lines on identical repaints

### VirtualTreeModel
- `QAbstractItemModel` backed by `std::vector<TreeNode>`
- Each node stores only `(byteOffset, depth, tagName, hasChildren, isLoaded)`
- Lazy child loading via `canFetchMore()` / `fetchMore()` / `hasChildren()`
- Three display columns: Element, Attributes, Text
- `byteOffsetFor(index)` for viewport sync
- `appendRootNodes()` for background population

## third_party / CMarkup
- `Markup.cpp` / `Markup.h` compiled as `libcmarkup.a`
- `MARKUP_ICONV` enabled on POSIX; `iconv` linked only on macOS (glibc has it built-in)
- Graceful stub target if sources are absent
