# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**loxe** is a native desktop XML editor for Linux and macOS (spiritual successor to Windows-only *foxe*). It targets arbitrarily large XML files (up to 2 GB) while keeping resident RAM under 80 MB. See [requirements.md](requirements.md) for the full SRS, [DONE.md](DONE.md) for implemented features with measured performance, and [TODO.md](TODO.md) for what is outstanding and for deliberate deviations from the design below.

## Build Commands

```bash
# Configure and build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Debug build (ASan + UBSan enabled by default)
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Run all tests
cmake --build build --target test

# Run a single test binary (Qt Test convention)
./build/tests/tst_PieceTable -v2
```

## Tech Stack Constraints

- **C++17** — minimum standard, no exceptions to this
- **Qt 6.4+** — Qt5 is explicitly not supported
- **CMarkup 11.5** — the only permitted third-party XML library; no libxml2, expat, pugixml, or any other. Note that the vendored copy exposes **no streaming file API** (`Load()` fails when `MDF_READFILE` is set; there is no public `Open()`), and `SetDoc()` needs the whole document resident. CMarkup is therefore used only for well-formedness validation of documents under 256 MB; all large-document traversal goes through our own `XmlScanner`.
- **CMake 3.24+** — build system; `vcpkg.json` pins dependency versions
- **No Scintilla/QScintilla** — text editor is a custom `QWidget` (see `ViewportRenderer`)
- **No Electron/CEF/web engine** — native Qt widgets only
- Qt6 must be dynamically linked (LGPL 3.0 compliance)

## Core Architecture

The components interact in a pipeline:

```
File on disk
    └─> MmapBuffer          — zero-copy mmap(PROT_READ, MAP_PRIVATE) with pread() fallback
            └─> PieceTable  — document representation: FILE buffer + ADD buffer, doubly-linked Piece list
                    ├─> SparseLineIndex    — (line, byte_offset) checkpoints every ~4 KB; O(log n) lookup
                    ├─> ViewportRenderer  — QWidget painting only ceil(height/line_height)+2 lines
                    │       └─> IncrementalHighlighter — XML tokeniser over viewport lines only
                    ├─> XmlScanner        — streaming node tokeniser; holds one node at a time
                    │       ├─> FormatEngine     — beautify / minify to a bounded sink
                    │       └─> VirtualTreeModel — QAbstractItemModel storing byte offsets only
                    └─> SearchEngine      — memmem over overlapping 1 MB windows
                            └─> AsyncLoader — QThread coordinating 3 phases: mmap → SparseLineIndex → tree level-1
```

### Key design invariants

- **Nothing reads the whole document.** Every full-document pass streams in bounded chunks and calls `PieceTable::releasePagesBefore()` as it advances — mapped pages count toward RSS while resident, so an unreleased 2 GB pass blows the memory budget on its own. The two exceptions are deliberate and capped at 256 MB: CMarkup validation and regex search.
- **Cursor position** is stored as a `uint64_t` byte offset, not (line, col), so it survives edits. Byte↔column conversion happens per visible line via a UTF-16-unit → byte map.
- **Tree nodes** store only byte offsets, depth and short display strings — never document content. Children are discovered by streaming that element's byte range via `fetchMore()`.
- **Everything derived from the document reads the PieceTable, not the file.** The line index and the tree both do, so they stay correct after edits. Re-reading the file on disk would show stale content.
- **Edits** go through PieceTable only: insert/delete/replace each push an undo record holding the piece list from *before* the edit. Compound operations (beautify, replace-all, encoding change) wrap themselves in `beginUndoGroup()`/`endUndoGroup()` to undo in a single step.
- **SparseLineIndex** invalidates and lazily rebuilds checkpoints after the edit position; it must never require a full rescan to answer a lookup. `attach()` makes it answer lookups before any scan, which is what lets the viewport paint within 1 s on a 2 GB file.
- **Thread model**: one writer thread (UI), multiple reader threads allowed via `std::shared_mutex` on PieceTable. Background threads: AsyncLoader, FormatEngine (for large files), Search (for files ≥ 100 MB), validation debounce (500 ms). Objects created on a worker thread must `moveToThread()` before being published to the UI.

### Performance targets (non-negotiable P0)

| Metric | Target |
|---|---|
| RAM for a freshly opened 2 GB file | ≤ 80 MB |
| Time to first visible content | < 1 s (any file size) |
| Tree level-1 population for 2 GB | < 3 s on SSD |
| Keystroke latency | < 16 ms |
| Scroll frame rate | ≥ 60 fps |
| Plain-text search throughput | ≥ 500 MB/s (SIMD `memmem` or equivalent) |
| Beautify throughput | ≥ 50 MB/s |

## Testing

Tests use **Qt Test** (`QTest`). Every test target runs with `QT_QPA_PLATFORM=offscreen` so the suite works headless. Shared fixtures live in `tests/TestHelpers.h`.

Existing coverage: `MmapBuffer`, `PieceTable`, `SparseLineIndex`, `FormatEngine`, `SearchEngine`, `XmlScanner`, `Encoding`, plus `tst_ViewportEditing`, which drives the editor through real `QTest::keyClick` / `keyClicks` events. The suite must stay clean under the Debug build's ASan + UBSan.

Two conventions worth knowing:

- Performance assertions call `LOXE_SKIP_IF_SANITIZED()` first — ASan is ~10× slower, so a throughput assertion there measures nothing and fails spuriously.
- The GUI is verified headlessly by rendering it: `loxe FILE --screenshot out.png --screenshot-delay MS`. Use it to confirm anything visual.

Still outstanding: a CI memory assertion for 2 GB files, 1 KB→2 GB regression fixtures, libFuzzer targets for `IncrementalHighlighter` and `VirtualTreeModel`, and benchmark regression alerts. See [TODO.md](TODO.md).

## Distribution

- **Linux**: AppImage via `linuxdeploy` with Qt plugin (`linuxdeploy-plugin-qt`)
- **macOS**: notarized DMG via `macdeployqt` + `create-dmg`
- CI: GitHub Actions on `ubuntu-22.04` and `macos-13`

## Session & Config File Locations

- Linux: `~/.config/loxe/loxe.conf`
- macOS: `~/Library/Preferences/com.loxe.app.plist`
