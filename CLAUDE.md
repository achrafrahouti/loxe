# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**loxe** is a native desktop XML editor for Linux and macOS (spiritual successor to Windows-only *foxe*). It targets arbitrarily large XML files (up to 2 GB) while keeping resident RAM under 80 MB. The project is currently in the **specification phase** — see [requirements.md](requirements.md) for the full SRS.

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
- **CMarkup 11.5** — the only permitted XML library; no libxml2, expat, pugixml, or any other
- **CMake 3.24+** — build system; `vcpkg.json` pins dependency versions
- **No Scintilla/QScintilla** — text editor is a custom `QWidget` (see `ViewportRenderer`)
- **No Electron/CEF/web engine** — native Qt widgets only
- Qt6 must be dynamically linked (LGPL 3.0 compliance)

## Core Architecture

The engine has seven major components that interact in a pipeline:

```
File on disk
    └─> MmapBuffer          — zero-copy mmap(PROT_READ, MAP_PRIVATE) with pread() fallback
            └─> PieceTable  — document representation: FILE buffer + ADD buffer, doubly-linked Piece list
                    ├─> SparseLineIndex    — (line, byte_offset) checkpoints every ~4 KB; O(log n) lookup
                    ├─> ViewportRenderer  — QWidget painting only ceil(height/line_height)+2 lines
                    │       └─> IncrementalHighlighter — XML tokeniser over viewport lines only
                    └─> VirtualTreeModel  — QAbstractItemModel storing (byte_offset, depth, tag_name) only
                            └─> AsyncLoader — QThread coordinating 3 phases: mmap → SparseLineIndex → tree level-1
```

`FormatEngine` is a separate streaming processor that reads through the PieceTable iterator and writes a new PieceTable (never loads the full document into a string).

### Key design invariants

- **Cursor position** is stored as `{piece_table_offset: uint64_t, visual_column: int}`, not (line, col), so it survives edits.
- **Tree nodes** store only `byte_offset` — content is fetched by seeking CMarkup to that offset on demand. Children are loaded lazily via `fetchMore()`.
- **Edits** go through PieceTable only: insert/delete/replace each push an undo record. All operations (beautify, replace-all, encoding change) must be a single undo step.
- **SparseLineIndex** invalidates and lazily rebuilds checkpoints after the edit position; it must never require a full rescan to answer a lookup.
- **Thread model**: one writer thread (UI), multiple reader threads allowed via `std::shared_mutex` on PieceTable. Background threads: AsyncLoader, FormatEngine (for large files), Search (for files ≥ 100 MB), validation debounce (500 ms).

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

Tests use **Qt Test** (`QTest`). Coverage must include `PieceTable`, `SparseLineIndex`, `MmapBuffer`, and `FormatEngine`. Regression tests open reference files from 1 KB to 2 GB. Memory tests assert resident RSS ≤ 80 MB for 2 GB files (measured via `/proc/self/status` on Linux, `task_info()` on macOS). Fuzz targets use libFuzzer against `IncrementalHighlighter` and `VirtualTreeModel`.

## Distribution

- **Linux**: AppImage via `linuxdeploy` with Qt plugin (`linuxdeploy-plugin-qt`)
- **macOS**: notarized DMG via `macdeployqt` + `create-dmg`
- CI: GitHub Actions on `ubuntu-22.04` and `macos-13`

## Session & Config File Locations

- Linux: `~/.config/loxe/loxe.conf`
- macOS: `~/Library/Preferences/com.loxe.app.plist`
