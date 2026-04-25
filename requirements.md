# loxe — Cross-Platform XML Editor
## Software Requirements Specification

**Version:** 1.0.0-draft  
**Status:** In progress  
**Targets:** Linux (x86_64, ARM64) · macOS (x86_64, Apple Silicon)  
**XML Engine:** CMarkup 11.5 (embedded)  
**Last updated:** 2026-04-25

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Stakeholders and Goals](#2-stakeholders-and-goals)
3. [Constraints](#3-constraints)
4. [Functional Requirements](#4-functional-requirements)
   - 4.1 [File I/O](#41-file-io)
   - 4.2 [Text Editor Viewport](#42-text-editor-viewport)
   - 4.3 [XML Tree View](#43-xml-tree-view)
   - 4.4 [Attribute Panel](#44-attribute-panel)
   - 4.5 [Navigation](#45-navigation)
   - 4.6 [Search and Replace](#46-search-and-replace)
   - 4.7 [Formatting and Beautify](#47-formatting-and-beautify)
   - 4.8 [Encoding Handling](#48-encoding-handling)
   - 4.9 [Validation](#49-validation)
   - 4.10 [Edit Operations](#410-edit-operations)
   - 4.11 [Session and Preferences](#411-session-and-preferences)
5. [Non-Functional Requirements](#5-non-functional-requirements)
   - 5.1 [Performance](#51-performance)
   - 5.2 [Memory](#52-memory)
   - 5.3 [Reliability](#53-reliability)
   - 5.4 [Usability](#54-usability)
   - 5.5 [Portability](#55-portability)
   - 5.6 [Security](#56-security)
6. [Architecture Requirements](#6-architecture-requirements)
   - 6.1 [MmapBuffer](#61-mmapbuffer)
   - 6.2 [PieceTable](#62-piecetable)
   - 6.3 [SparseLineIndex](#63-sparselineindex)
   - 6.4 [ViewportRenderer](#64-viewportrenderer)
   - 6.5 [IncrementalHighlighter](#65-incrementalhighlighter)
   - 6.6 [VirtualTreeModel](#66-virtualtreemodel)
   - 6.7 [AsyncLoader](#67-asyncloader)
   - 6.8 [FormatEngine](#68-formatengine)
7. [Data Requirements](#7-data-requirements)
8. [External Interface Requirements](#8-external-interface-requirements)
9. [Build and Packaging Requirements](#9-build-and-packaging-requirements)
10. [Testing Requirements](#10-testing-requirements)
11. [Glossary](#11-glossary)

---

## 1. Introduction

### 1.1 Purpose

This document specifies the functional and non-functional requirements for **loxe**, an open-source XML editor for Linux and macOS. loxe is designed as a direct spiritual successor to the Windows-only *firstobject XML editor (foxe)*, built on the same CMarkup C++ library and targeting the same class of users: developers and data engineers who need to inspect, edit, and format arbitrarily large XML files with minimal tooling overhead.

### 1.2 Scope

loxe is a native desktop GUI application. It is not a web application, a language-server plugin, or a library. It provides:

- A dual-pane interface (raw text editor + live XML tree view)
- High-performance handling of XML files up to **2 GB** in size
- Full round-trip editing: changes made in either pane are reflected in the other
- A small binary footprint with no runtime dependency on Java, Python, or an external XML library beyond the embedded CMarkup

### 1.3 Definitions

| Term | Meaning |
|---|---|
| **loxe** | The application described in this document |
| **CMarkup** | The embedded C++ XML parsing library (Release 11.5, firstobject) |
| **EDOM** | Efficient Document Object Model — CMarkup's position-based navigation model |
| **Piece Table** | A document representation storing edits as sequences of spans over two buffers (FILE, ADD) |
| **mmap** | POSIX `mmap()` — maps a file into virtual address space without reading it into RAM |
| **SparseLineIndex** | A sampled index of newline positions enabling O(log n + ε) line-number lookup |
| **Viewport** | The visible region of the text editor, typically 40–80 lines |
| **Large file** | Any file ≥ 100 MB |
| **Huge file** | Any file ≥ 500 MB |
| **Giant file** | Any file approaching or at the 2 GB target ceiling |

---

## 2. Stakeholders and Goals

### 2.1 Primary user

A developer or data engineer who routinely opens multi-megabyte to multi-gigabyte XML files (data exports, log dumps, configuration manifests, DITA documents) and needs to:

- Quickly inspect the tree structure without waiting for a full parse
- Edit specific elements or attributes in place
- Reformat badly indented XML
- Search for text or XPath patterns in a large document
- Convert between encodings

### 2.2 Design goals

| Priority | Goal |
|---|---|
| P0 | Open a 2 GB XML file in under 3 seconds with UI responsive throughout |
| P0 | Never allocate 2 GB of RAM regardless of file size |
| P0 | Run identically on Linux and macOS from a single source tree |
| P1 | Match foxe's core feature set: tree view, syntax color, path display, beautify, encoding |
| P1 | Single portable binary (AppImage on Linux, .app bundle on macOS) |
| P2 | Extensible for future scripting (FOAL-compatible API surface) |
| P2 | Accessible keyboard-driven workflow |

---

## 3. Constraints

### 3.1 Technical constraints

- **C++17** minimum language standard
- **Qt 6.4+** for the GUI framework (Qt5 not supported)
- **CMarkup 11.5** is the only permitted XML parsing library; no libxml2, no expat, no pugixml
- **No Electron, no CEF, no web engine** — native Qt widgets only
- **No Scintilla / QScintilla** — the text editor is a custom QWidget (see section 6.4)
- CMake 3.24+ as the build system
- No dependencies outside Qt6, CMarkup, and the C++ standard library for the core engine

### 3.2 Platform constraints

| Platform | Minimum OS version | Architecture |
|---|---|---|
| Linux | Kernel 5.4, glibc 2.31 | x86_64, aarch64 |
| macOS | macOS 12 (Monterey) | x86_64, arm64 (Apple Silicon) |

### 3.3 Legal constraints

- CMarkup is used under the terms of the firstobject license (commercial use requires written permission — verify before any commercial distribution)
- Qt6 is used under LGPL 3.0; dynamic linking required to comply
- The application must not bundle or require any GPL-licensed component that would affect the application's license

---

## 4. Functional Requirements

### 4.1 File I/O

| ID | Requirement |
|---|---|
| FIO-01 | The application shall open any XML file up to 2 GB in size |
| FIO-02 | File opening shall use `mmap()` on Linux and macOS for files ≥ 1 MB; smaller files may be read into memory with `read()` |
| FIO-03 | File opening shall be performed on a background thread; the UI shall remain responsive (non-frozen) during the entire load operation |
| FIO-04 | A progress indicator (progress bar + estimated time remaining) shall be displayed while the `SparseLineIndex` is being built |
| FIO-05 | The application shall support opening files via the File menu, drag-and-drop onto the window, and a command-line argument (`loxe /path/to/file.xml`) |
| FIO-06 | Save shall use an atomic write strategy: write to a temporary file in the same directory, then `rename()` atomically. The original file must never be left in a partially-written state |
| FIO-07 | For files ≥ 500 MB the application shall offer the user a choice between **in-place patch** (only changed regions rewritten, instant for small edits) and **full rewrite** (safe but slow). The choice shall be persisted as a preference |
| FIO-08 | The application shall detect when an open file has been modified externally (using `QFileSystemWatcher`) and prompt the user to reload or keep the in-memory version |
| FIO-09 | A **recent files** list of at least 20 entries shall be maintained and displayed in the File menu |
| FIO-10 | The application shall support creating a new empty XML document |
| FIO-11 | The application shall correctly read and write files with a UTF-8 BOM, UTF-16 LE BOM, UTF-16 BE BOM, and no BOM, as determined by the `CMarkup::DetectUTF8` and `CMarkup::GetDeclaredEncoding` utilities |

### 4.2 Text Editor Viewport

| ID | Requirement |
|---|---|
| TVP-01 | The text editor shall be implemented as a custom `QWidget` subclass (`ViewportRenderer`) that paints only the lines currently visible on screen |
| TVP-02 | The visible region shall be at most `ceil(widget_height / line_height) + 2` lines tall (one line overshoot top and bottom for smooth scrolling) |
| TVP-03 | Vertical scrolling shall be implemented by changing the first-visible-line offset; it shall never require scanning the file sequentially |
| TVP-04 | The total line count shown in the scroll bar shall be an estimate updated progressively as the `SparseLineIndex` is built; the estimate shall be accurate to ±1 line once the index is complete |
| TVP-05 | Horizontal scrolling shall be supported; the horizontal scroll bar shall appear only when the longest visible line exceeds the widget width |
| TVP-06 | The editor shall support text cursor movement: arrow keys, Home, End, Page Up, Page Down, Ctrl+Home, Ctrl+End |
| TVP-07 | The editor shall support selection via Shift + any cursor movement key, and mouse click-drag selection |
| TVP-08 | The editor shall support standard clipboard operations: Cut (Ctrl+X), Copy (Ctrl+C), Paste (Ctrl+V), Select All (Ctrl+A) |
| TVP-09 | Line numbers shall be displayed in a gutter to the left of the text, updated to reflect the true line number (not a viewport-relative number) |
| TVP-10 | The current line shall be highlighted with a subtle background color |
| TVP-11 | The editor shall display a vertical marker (thin line) at column 80 and column 120 (configurable, can be disabled) |
| TVP-12 | Word wrap shall be toggleable via View menu and keyboard shortcut (Alt+Z). In word-wrap mode the `SparseLineIndex` tracks visual lines, not file newlines |
| TVP-13 | Matching XML tag pairs shall be highlighted when the cursor is inside a tag name |

### 4.3 XML Tree View

| ID | Requirement |
|---|---|
| TRV-01 | The tree view shall be a `QTreeView` backed by a custom `VirtualTreeModel` that stores only `(byte_offset, depth, tag_name)` tuples, never full node content |
| TRV-02 | The root level of the tree shall be populated during the initial background parse pass; children shall be loaded lazily when the user expands a node |
| TRV-03 | Expanding a tree node shall seek CMarkup to the stored `byte_offset` and parse only that subtree, not the full document |
| TRV-04 | The tree shall display element tag names, a truncated preview of text content (≤ 60 chars), and attribute count |
| TRV-05 | Clicking a tree node shall scroll the text editor to the corresponding line and highlight the opening tag |
| TRV-06 | Double-clicking a tree node shall open an inline editor for that element's text content |
| TRV-07 | Right-clicking a tree node shall show a context menu with: Copy XPath, Copy tag name, Add child element, Add sibling element, Delete element, Copy subtree as XML |
| TRV-08 | The tree view shall support keyboard navigation: arrow keys to move, Enter to expand/collapse, Delete to remove the selected node (with confirmation) |
| TRV-09 | The tree and text editor shall be kept in sync: edits in either pane shall be reflected in the other within 200 ms |
| TRV-10 | For files ≥ 500 MB the full tree depth shall not be pre-built; only the first two levels shall be indexed at open time |

### 4.4 Attribute Panel

| ID | Requirement |
|---|---|
| ATP-01 | A dockable panel shall display the attributes of the currently selected XML element as a two-column table (name / value) |
| ATP-02 | Attribute values shall be directly editable in-place within the table; pressing Enter commits the change |
| ATP-03 | New attributes may be added via an "Add attribute" button or by pressing Insert while the panel is focused |
| ATP-04 | Attributes may be deleted by selecting the row and pressing Delete (with confirmation if the value is non-empty) |
| ATP-05 | The panel shall display the full XPath of the currently selected element at the top |
| ATP-06 | Attribute panel changes shall immediately update the underlying PieceTable and be reflected in the text editor viewport |

### 4.5 Navigation

| ID | Requirement |
|---|---|
| NAV-01 | A breadcrumb bar at the top of the window shall display the XPath from the document root to the element under the cursor, updated as the cursor moves |
| NAV-02 | Clicking any segment of the breadcrumb shall jump the tree view to that ancestor element |
| NAV-03 | Go To Line (Ctrl+G) shall open a dialog accepting a line number and jumping the viewport to that line; for huge files the jump shall be instant (O(log n)) |
| NAV-04 | Go To Byte Offset shall be available in the navigation menu for low-level inspection of large binary/mixed-encoding files |
| NAV-05 | The application shall maintain a navigation history (back/forward, like a browser) — Alt+Left and Alt+Right — tracking cursor positions across jumps |
| NAV-06 | Bookmarks shall be supported: Ctrl+B adds a bookmark at the current line; bookmarks are listed in a panel and persisted per-file in the session store |

### 4.6 Search and Replace

| ID | Requirement |
|---|---|
| SRC-01 | Find (Ctrl+F) shall open an inline search bar at the bottom of the text editor |
| SRC-02 | Find & Replace (Ctrl+H) shall open a dialog with separate find and replace fields |
| SRC-03 | Search shall support: plain text, case-insensitive text, and POSIX extended regular expressions |
| SRC-04 | XPath-based search (Ctrl+Shift+F) shall allow the user to enter an XPath 1.0 expression; matching nodes shall be highlighted in both the tree and the text editor |
| SRC-05 | For files ≥ 100 MB, search shall be performed in a background thread; results shall stream into a results panel as found, without blocking the UI |
| SRC-06 | "Find next" (F3) and "Find previous" (Shift+F3) shall wrap around the document |
| SRC-07 | Replace All shall report the number of replacements made and shall be undoable as a single undo step |
| SRC-08 | Search results shall be highlighted in the viewport with a distinct background color; the current match shall use a different highlight from other matches |

### 4.7 Formatting and Beautify

| ID | Requirement |
|---|---|
| FMT-01 | The application shall provide a **Beautify / Format** command (Ctrl+Shift+B) that re-indents the entire document with configurable indent style (spaces or tabs) and indent width (2 / 4 / 8) |
| FMT-02 | Beautify shall preserve all content including comments, CDATA sections, processing instructions, and declared encoding |
| FMT-03 | Beautify shall be implemented by the `FormatEngine` which reads the document via CMarkup in streaming mode and writes a new formatted version; it shall not require loading the full document into memory at once |
| FMT-04 | For files ≥ 100 MB, beautify shall run in a background thread and report progress |
| FMT-05 | A **Minify** command shall produce the opposite: collapse all insignificant whitespace between tags |
| FMT-06 | The application shall detect and display (but not automatically fix) mixed indentation (tabs and spaces on the same level) |

### 4.8 Encoding Handling

| ID | Requirement |
|---|---|
| ENC-01 | The application shall detect encoding from, in order: UTF-16 BOM, UTF-8 BOM, XML declaration `encoding=` attribute, and finally UTF-8 auto-detection using `CMarkup::DetectUTF8` |
| ENC-02 | The current encoding shall be displayed in the status bar at all times |
| ENC-03 | The user may change the encoding via a menu command; the application shall convert the in-memory representation and update the XML declaration |
| ENC-04 | Supported encodings for open and save shall include at minimum: UTF-8, UTF-8 with BOM, UTF-16 LE, UTF-16 BE, ISO-8859-1, and the system locale encoding |
| ENC-05 | The application shall display a non-ASCII character's Unicode code point (U+XXXX), UTF-8 hex bytes, and Unicode block name in the status bar when the cursor is on that character |
| ENC-06 | A **Convert Char Ref** command shall convert a numeric character reference (`&#8364;`, `&#x20AC;`) to the actual Unicode character and vice-versa |

### 4.9 Validation

| ID | Requirement |
|---|---|
| VAL-01 | The application shall continuously validate that the document is **well-formed XML** and display the result in the status bar (green check / red X with error message and line number) |
| VAL-02 | Well-formedness validation shall use CMarkup's `IsWellFormed()` and shall run in a background thread with a debounce delay of 500 ms after the last edit |
| VAL-03 | Validation errors shall be shown as red squiggles in the text editor gutter and as an entry in an "Errors" panel |
| VAL-04 | The application shall provide **DTD validation** as an optional command if a DOCTYPE declaration referencing a local or accessible DTD is present |
| VAL-05 | The application shall never crash or hang on malformed XML; it shall display and allow editing of the malformed content |

### 4.10 Edit Operations

| ID | Requirement |
|---|---|
| EDT-01 | All edits shall be implemented via PieceTable operations: insert, delete, and replace, each recorded as undo steps |
| EDT-02 | Undo (Ctrl+Z) and Redo (Ctrl+Y / Ctrl+Shift+Z) shall be unlimited in depth, bounded only by available RAM for the undo stack |
| EDT-03 | Undo shall be available for all operations including beautify, replace-all, encoding conversion, and attribute edits |
| EDT-04 | A **Tag wrap** command shall wrap the selected text in a new XML element whose name the user provides |
| EDT-05 | A **Tag unwrap** command shall remove the tag surrounding the cursor, keeping the inner content |
| EDT-06 | Auto-closing of XML tags: when the user types `>` to close an opening tag, the corresponding closing tag shall be inserted automatically (configurable) |
| EDT-07 | Auto-completion shall suggest tag names and attribute names from the current document vocabulary when the user types `<` or a space inside a tag |

### 4.11 Session and Preferences

| ID | Requirement |
|---|---|
| SES-01 | Window geometry, pane split position, and sidebar visibility shall be persisted across sessions using `QSettings` |
| SES-02 | The application shall offer to reopen the last-open file (or all last-open files if tabs are supported) on next launch |
| SES-03 | Per-file state shall be stored: scroll position, cursor position, bookmarks, word-wrap state |
| SES-04 | A preferences dialog (Ctrl+,) shall allow configuration of: font family and size, indent style, color theme (light / dark / system), column markers, auto-close tags, recent file count, and default save strategy for large files |
| SES-05 | The application shall support at least two built-in color themes: **Light** and **Dark**; it shall follow the system theme by default |

---

## 5. Non-Functional Requirements

### 5.1 Performance

| ID | Requirement | Measurement condition |
|---|---|---|
| PF-01 | Time from launch to first usable window | < 400 ms on a 2020-era machine |
| PF-02 | Time from "open" command to first visible content (any size file) | < 1 second (mmap returns immediately; first paint follows) |
| PF-03 | Time to full tree level-1 population for a 2 GB file | < 3 seconds on SSD, < 10 seconds on HDD |
| PF-04 | Keystroke-to-screen latency (typing in the editor) | < 16 ms (60 fps budget) |
| PF-05 | Scroll frame rate at any position in any size file | ≥ 60 fps |
| PF-06 | Beautify throughput | ≥ 50 MB/s (i.e., a 2 GB file formatted in ≤ 40 s) |
| PF-07 | Plain text search throughput on raw file bytes | ≥ 500 MB/s using SIMD-optimised `memmem` or equivalent |
| PF-08 | Go-to-line response time | < 5 ms for any line in any file size |

### 5.2 Memory

| ID | Requirement |
|---|---|
| MM-01 | Resident RAM for a freshly opened 2 GB file (no edits) shall not exceed **80 MB** |
| MM-02 | Resident RAM shall not grow proportionally to file size; it shall remain bounded regardless of file size |
| MM-03 | The PieceTable ADD buffer (typed edits) shall not exceed **256 MB** per session; if the limit is approached the user shall be warned |
| MM-04 | The SparseLineIndex shall not exceed **4 MB** for any file under 2 GB |
| MM-05 | The undo stack shall be bounded to **512 MB** by default; older entries shall be dropped when the limit is reached, with a status bar notification |

### 5.3 Reliability

| ID | Requirement |
|---|---|
| REL-01 | The application shall not crash on any valid or malformed XML file regardless of size |
| REL-02 | The application shall not crash if `mmap()` fails (e.g., low virtual address space); it shall fall back to `pread()`-based chunked reading |
| REL-03 | The atomic save strategy (temp + rename) shall guarantee that no partial-write state is ever left on disk |
| REL-04 | If a background thread encounters an unhandled exception it shall log the error and terminate only that thread, not the whole application |
| REL-05 | The application shall auto-save a crash recovery file every 60 seconds (configurable) when unsaved changes exist; the recovery file is offered at next launch |

### 5.4 Usability

| ID | Requirement |
|---|---|
| USA-01 | The application shall be fully operable by keyboard alone (no action requires a mouse) |
| USA-02 | All menu items shall have keyboard shortcuts documented in the menu label |
| USA-03 | The status bar shall always show: current line/column, total lines, file size, encoding, and well-formedness status |
| USA-04 | Long operations (beautify, search on large files, initial index build) shall always display a progress indicator with a Cancel button |
| USA-05 | Error messages shall describe what went wrong, on which line/byte if applicable, and what the user can do about it |
| USA-06 | The UI shall follow the native platform's HIG (Human Interface Guidelines) for macOS and GTK/GNOME conventions on Linux where possible within Qt |

### 5.5 Portability

| ID | Requirement |
|---|---|
| PRT-01 | A single CMake configuration shall produce a working build on both Linux and macOS without platform-specific preprocessor guards in application code (only in the engine layer) |
| PRT-02 | The Linux build shall be distributable as an **AppImage** containing all Qt libraries; no system Qt installation shall be required |
| PRT-03 | The macOS build shall be distributable as a signed and notarized **.app bundle** in a DMG |
| PRT-04 | All file paths, including those stored in settings and session files, shall be stored as UTF-8 strings; no platform path separator shall be hardcoded |
| PRT-05 | The application shall run correctly with both X11 and Wayland compositors on Linux |

### 5.6 Security

| ID | Requirement |
|---|---|
| SEC-01 | The application shall not execute any content from opened XML files (no scripting, no external entity expansion by default) |
| SEC-02 | External entity references (XXE) shall be disabled by default; the user must explicitly enable them via a preference |
| SEC-03 | The application shall not make any network connections unless the user explicitly requests loading a remote URL via a designated command |
| SEC-04 | Temporary files created during save operations shall be created with permissions 0600 (owner read/write only) |

---

## 6. Architecture Requirements

### 6.1 MmapBuffer

| ID | Requirement |
|---|---|
| ARC-MB-01 | `MmapBuffer` shall open a file read-only with `open(O_RDONLY)` and map it with `mmap(PROT_READ, MAP_PRIVATE)` |
| ARC-MB-02 | `MmapBuffer` shall call `madvise(MADV_SEQUENTIAL)` on the mapping during initial index build, then `madvise(MADV_RANDOM)` for interactive use |
| ARC-MB-03 | `MmapBuffer` shall expose a `std::string_view slice(off_t offset, size_t len)` method returning a zero-copy view into the mapping |
| ARC-MB-04 | `MmapBuffer` shall provide a `pread()`-based fallback path activated when `mmap()` returns `MAP_FAILED` |
| ARC-MB-05 | The mmap mapping shall be refreshed (remapped) after an external file change is detected, with user confirmation |

### 6.2 PieceTable

| ID | Requirement |
|---|---|
| ARC-PT-01 | The PieceTable shall represent the document as a doubly-linked list of `Piece` nodes, each containing `{origin: FILE|ADD, offset: uint64_t, length: uint64_t}` |
| ARC-PT-02 | Insert at position P shall: find the piece containing P (binary search on prefix-sum), split it if P is not at a boundary, and insert a new ADD piece |
| ARC-PT-03 | Delete of range [P, Q] shall: split pieces at P and Q, then unlink all pieces in between |
| ARC-PT-04 | Each edit operation shall push a `{piece_list_snapshot, cursor_before, cursor_after}` record onto the undo stack |
| ARC-PT-05 | The PieceTable shall expose an iterator interface for sequential reading (used by FormatEngine and search) |
| ARC-PT-06 | The PieceTable shall be thread-safe for one writer and multiple readers using a reader-writer lock (`std::shared_mutex`) |

### 6.3 SparseLineIndex

| ID | Requirement |
|---|---|
| ARC-LI-01 | The index shall store `(line_number, byte_offset)` checkpoints at approximately every 4 KB of file content |
| ARC-LI-02 | The index shall be built on a background `QThread` that is cancellable; cancellation shall be checked at each checkpoint write |
| ARC-LI-03 | `line_to_offset(uint64_t line) -> off_t` shall binary-search checkpoints then scan forward byte-by-byte; total latency ≤ 5 ms for any file |
| ARC-LI-04 | `offset_to_line(off_t offset) -> uint64_t` shall binary-search checkpoints then scan forward; total latency ≤ 5 ms for any file |
| ARC-LI-05 | The index shall handle CR, LF, and CRLF line endings, counting each as exactly one line |
| ARC-LI-06 | After a PieceTable edit the index shall invalidate checkpoints at and after the edit position and rebuild them lazily on the next lookup |

### 6.4 ViewportRenderer

| ID | Requirement |
|---|---|
| ARC-VP-01 | `ViewportRenderer` shall be a `QWidget` subclass overriding `paintEvent`, `keyPressEvent`, `mousePressEvent`, `mouseMoveEvent`, and `wheelEvent` |
| ARC-VP-02 | `paintEvent` shall fetch at most `viewport_lines + 2` lines from the PieceTable via SparseLineIndex; it shall never read the entire file |
| ARC-VP-03 | All text rendering shall use `QPainter::drawText` with the configured monospace font; no third-party text layout library is permitted |
| ARC-VP-04 | Cursor position shall be stored as `{piece_table_offset: uint64_t, visual_column: int}` — not a (line, col) pair — so it remains stable across edits |
| ARC-VP-05 | The widget shall emit a `cursorMoved(uint64_t byte_offset)` signal on every cursor movement for the tree view and breadcrumb bar to consume |
| ARC-VP-06 | Tab characters shall be rendered as spaces with configurable tab stop width (default: 4) |

### 6.5 IncrementalHighlighter

| ID | Requirement |
|---|---|
| ARC-HL-01 | The highlighter shall tokenise only the lines visible in the current viewport, plus a context window of 10 lines above to establish parser state |
| ARC-HL-02 | The highlighter shall implement an XML token state machine with the following token classes: tag name, attribute name, attribute value, text content, comment, CDATA section, processing instruction, DOCTYPE, and entity reference |
| ARC-HL-03 | Token colors shall be configurable per color theme |
| ARC-HL-04 | The highlighter shall be invoked from the `paintEvent` path synchronously; it shall complete within the 16 ms frame budget |
| ARC-HL-05 | The highlighter shall cache the state at the top of the current viewport so that state is not recomputed on identical repaints |

### 6.6 VirtualTreeModel

| ID | Requirement |
|---|---|
| ARC-TM-01 | `VirtualTreeModel` shall implement `QAbstractItemModel` using an internal node array of `{byte_offset, depth, tag_name, has_children, is_loaded}` |
| ARC-TM-02 | `hasChildren()` shall return true for any node with `has_children == true` even if children are not yet loaded; this enables the tree to show expand arrows lazily |
| ARC-TM-03 | When `fetchMore()` is called for a node, CMarkup shall seek to `byte_offset` using `MDF_READFILE` and parse forward until depth returns to the parent depth, emitting child nodes |
| ARC-TM-04 | The model shall use CMarkup's `SavePos` and `RestorePos` to allow simultaneous navigation of multiple subtrees without full reparse |
| ARC-TM-05 | Node data displayed shall include: tag name (column 0), attribute summary `[@attr1="..." ...]` (column 1), and text preview (column 2) |

### 6.7 AsyncLoader

| ID | Requirement |
|---|---|
| ARC-AL-01 | `AsyncLoader` shall run on a dedicated `QThread` and coordinate three sequential phases: (1) mmap, (2) SparseLineIndex build, (3) VirtualTreeModel level-1 parse |
| ARC-AL-02 | At the end of phase 1, the main thread shall receive the `MmapBuffer` and begin displaying the file in the viewport (with an incomplete index) |
| ARC-AL-03 | At the end of phase 2, the viewport scroll bar range shall be updated to the final line count |
| ARC-AL-04 | The loader shall emit progress signals (`loadProgress(int percent, QString phase)`) at least every 100 ms |
| ARC-AL-05 | The loader shall be cancellable at any phase boundary; cancellation leaves the file closed and memory freed |

### 6.8 FormatEngine

| ID | Requirement |
|---|---|
| ARC-FE-01 | `FormatEngine` shall read input via PieceTable iterator (not by loading the full document into a string) |
| ARC-FE-02 | `FormatEngine` shall write formatted output to a new `PieceTable` (replacing the current one) using a streaming approach with a configurable output buffer of ≤ 8 MB |
| ARC-FE-03 | The engine shall correctly handle all CMarkup node types: `MNT_ELEMENT`, `MNT_TEXT`, `MNT_CDATA_SECTION`, `MNT_COMMENT`, `MNT_PROCESSING_INSTRUCTION`, `MNT_DOCUMENT_TYPE` |
| ARC-FE-04 | The entire beautify operation shall be a single undo step |

---

## 7. Data Requirements

### 7.1 Session file format

Session data shall be stored using `QSettings` in the platform-native location:
- Linux: `~/.config/loxe/loxe.conf`
- macOS: `~/Library/Preferences/com.loxe.app.plist`

### 7.2 File format support

The application shall open and correctly display:

- Well-formed XML 1.0 and XML 1.1 documents
- HTML and XHTML (treated as loosely-formed XML; no automatic repair)
- Any text file (shown as raw text with no tree view if not parseable as XML)

### 7.3 Temporary files

All temporary files shall be written to the same directory as the source file (to ensure `rename()` is atomic across filesystems) and shall be deleted on successful save or application exit.

---

## 8. External Interface Requirements

### 8.1 Command-line interface

```
loxe [OPTIONS] [FILE]

Options:
  --line N       Open FILE and jump to line N
  --search TERM  Open FILE and trigger search for TERM
  --ro           Open FILE read-only (disables all edit commands)
  --version      Print version and exit
  --help         Print help and exit
```

### 8.2 Desktop integration (Linux)

- A `.desktop` file shall be provided for launcher integration
- MIME type `application/xml` and `text/xml` shall be registered
- AppStream metadata shall be provided for software-center discovery

### 8.3 Desktop integration (macOS)

- The app bundle shall declare `com.apple.xml` as a handled UTI
- Document-based application lifecycle (re-open file from Finder, drag onto Dock icon) shall be supported via `QFileOpenEvent`

---

## 9. Build and Packaging Requirements

| ID | Requirement |
|---|---|
| BLD-01 | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` shall produce a working binary on both platforms with no additional steps |
| BLD-02 | A `vcpkg.json` manifest shall pin all external dependency versions |
| BLD-03 | The GitHub Actions CI matrix shall build and run tests on `ubuntu-22.04` and `macos-13` on every push to `main` and every pull request |
| BLD-04 | The Linux release artifact shall be an AppImage produced by `linuxdeploy` with Qt plugin |
| BLD-05 | The macOS release artifact shall be a notarized DMG produced by `macdeployqt` + `create-dmg` |
| BLD-06 | Build time for a clean release build shall not exceed 5 minutes on a 4-core CI runner |
| BLD-07 | Debug builds shall enable address sanitizer (ASan) and undefined-behaviour sanitizer (UBSan) by default |

---

## 10. Testing Requirements

| ID | Requirement |
|---|---|
| TST-01 | Unit tests shall be written using Qt Test and shall cover all methods of `PieceTable`, `SparseLineIndex`, `MmapBuffer`, and `FormatEngine` |
| TST-02 | A regression test suite shall open, parse, and display a set of reference XML files including: a 1 KB minimal file, a 10 MB document, a 500 MB document, and a synthetic 2 GB document |
| TST-03 | All 2 GB file tests shall assert that resident RAM never exceeds 80 MB at any point (measured via `/proc/self/status` on Linux and `task_info()` on macOS) |
| TST-04 | Round-trip tests shall verify that open → beautify → save → open produces byte-identical output for a defined set of test files |
| TST-05 | Fuzz testing shall be applied to the `IncrementalHighlighter` and `VirtualTreeModel` using libFuzzer with a corpus of malformed XML fragments |
| TST-06 | Performance benchmarks shall be run in CI and a regression alert raised if any PF-* metric degrades by more than 10% |
| TST-07 | UI smoke tests shall use `QTest::mouseClick` / `QTest::keyClick` to verify file open, basic editing, save, and undo on a 10 MB test file |

---

## 11. Glossary

| Term | Definition |
|---|---|
| **ADD buffer** | The append-only in-memory buffer in the PieceTable that stores characters typed by the user |
| **AppImage** | A self-contained Linux application format requiring no installation |
| **CDATA section** | An XML construct (`<![CDATA[...]]>`) containing character data that is not parsed as markup |
| **CMarkup** | The C++ XML library embedded in loxe, providing EDOM navigation and streaming file I/O |
| **EDOM** | Efficient Document Object Model — CMarkup's position-based, non-allocating navigation API |
| **FILE buffer** | The read-only memory-mapped region of the original file in the PieceTable |
| **FormatEngine** | The loxe component responsible for beautify and minify operations |
| **gap buffer** | A classic text editor data structure: a contiguous string with a movable gap. Efficient for local edits, catastrophic for 2 GB files |
| **HIG** | Human Interface Guidelines — platform design standards for macOS (Apple) and GNOME |
| **iconv** | POSIX library for character encoding conversion, used by CMarkup on Linux and macOS (`MARKUP_ICONV`) |
| **mmap** | POSIX `mmap()` — maps a file region into virtual address space. The OS pages in physical memory lazily |
| **madvise** | POSIX advisory call to hint the kernel about expected access pattern for a memory-mapped region |
| **off_t** | POSIX 64-bit file offset type on Linux and macOS (`_FILE_OFFSET_BITS=64`) |
| **Piece** | A single record in the PieceTable: `{origin, offset, length}` |
| **PieceTable** | A document representation as a sequence of Pieces referencing FILE and ADD buffers |
| **SparseLineIndex** | A sampled array of `(line_number, byte_offset)` pairs enabling O(log n) line lookup |
| **ViewportRenderer** | The custom `QWidget` subclass that paints only the visible lines of the document |
| **VirtualTreeModel** | A `QAbstractItemModel` that stores only byte offsets into the file, loading subtree data on demand |
| **well-formed** | An XML document satisfying all syntactic rules of the XML 1.0 or 1.1 specification |
| **XXE** | XML External Entity — an XML feature sometimes exploited to read local files; disabled by default in loxe |