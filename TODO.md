# TODO — Features Not Yet Implemented

Items are grouped by component. Priority follows the SRS: P0 > P1 > P2.
See [DONE.md](DONE.md) for what is implemented and measured.

---

## Known design deviations

These are deliberate departures from the original design in
[CLAUDE.md](CLAUDE.md) / [requirements.md](requirements.md), recorded so they
are not "fixed" by accident.

- **The tree does not use CMarkup.** The vendored CMarkup 11.5 has no public
  streaming file API: `CMarkup::Load()` returns `false` outright when
  `MDF_READFILE` is set, and no `Open()` is declared. The documented "seek
  CMarkup to `byte_offset`" approach is therefore not implementable with this
  library, and `SetDoc()` would need the whole document resident, which the
  80 MB budget forbids. `src/engine/XmlScanner.*` provides the streaming
  tokeniser instead; CMarkup is used only for well-formedness validation.
  Revisit if CMarkup is upgraded to a build that exposes its file mode.
- **Tree expansion is bounded.** Enumerating an element's children means
  traversing its whole subtree, so expansion stops after 50,000 children or
  768 MB scanned and the parent is flagged partial. Without the byte cap, a 2 GB
  document whose root has a few enormous children takes >4 s to expand.
  A "load more" affordance would be the proper fix.
- **Word wrap is a stored flag only.** Honouring it requires SparseLineIndex to
  track visual rather than physical lines (TVP-12); the toggle currently changes
  nothing on screen.
- **Beautify reflows mixed content.** `<p>a <b>c</b> d</p>` is re-indented onto
  separate lines. Standard for XML beautifiers, wrong for whitespace-sensitive
  formats such as XHTML.
- **Reformat and regex search are capped at 256 MB** because both materialise
  the document (reformat to keep the operation a single undo step, regex because
  `QRegularExpression` needs a `QString`). Larger documents are refused with an
  explanation rather than silently degrading.

---

## Engine

### PieceTable
- [ ] Piece-list compaction: long editing sessions grow the list without bound
      since only adjacent ADD pieces coalesce
- [ ] Persist the ADD buffer for crash recovery (REL-05)

### SparseLineIndex
- [ ] Word-wrap mode: track visual lines instead of file newlines (TVP-12)
- [ ] Concurrent `extendTo()` from the UI thread duplicates work already in
      flight on the loader thread; harmless but wasteful

### FormatEngine
- [ ] Mixed indentation detection and display (FMT-06)
- [ ] Preserve mixed content inline instead of reflowing it
- [ ] Stream reformat output to a temp file so documents > 256 MB can be
      reformatted (currently refused)

### SearchEngine
- [ ] Background search thread for files ≥ 100 MB with a streaming results panel
- [ ] Whole-word and in-selection scoping

---

## UI

### ViewportRenderer
- [ ] Matching tag-pair highlight when the cursor is inside a tag name (TVP-13)
- [ ] Word wrap rendering (TVP-12)
- [ ] Auto-close tags on `>` (EDT-06, configurable)
- [ ] Ctrl+Left/Right word-wise cursor movement
- [ ] Proportional-font support: hit-testing assumes a fixed cell grid, so a
      non-monospace font would misplace the caret
- [ ] Overwrite (Insert key) mode

### VirtualTreeModel
- [ ] "Load more children" for partially-loaded nodes
- [ ] Rebuild or patch the tree after edits — it currently keeps byte offsets
      from the last load, so they drift once the document is edited above them
- [ ] Fuzz target via libFuzzer

### IncrementalHighlighter
- [ ] Per-theme configuration via the preferences dialog
- [ ] Fuzz target via libFuzzer

### MainWindow
- [ ] Attribute panel in-place editing committing to the PieceTable
- [ ] Breadcrumb segments clickable to jump to the ancestor element
- [ ] XPath 1.0 search (Ctrl+Shift+F) with tree + viewport highlight
- [ ] Replace All for regex searches (plain text only today)
- [ ] Errors panel and red squiggle gutter markers for validation failures
- [ ] DTD validation command when a DOCTYPE is present
- [ ] Restore the caret reliably after an external-change reload (currently a
      best-effort `singleShot`)

### Navigation (NAV-*)
- [ ] Go To Byte Offset command
- [ ] Navigation history back/forward (Alt+Left / Alt+Right)
- [ ] Bookmarks: Ctrl+B adds, bookmark panel lists, persisted per file

### Encoding (ENC-*)
- [ ] Change encoding command (convert in memory + update the XML declaration)
- [ ] Decode UTF-16 documents for display — detection reports them, but the
      viewport still interprets bytes as UTF-8
- [ ] Unicode code point / UTF-8 bytes / block name in the status bar
- [ ] Convert Char Ref command (`&#8364;` ↔ `€`)

### Edit operations (EDT-*)
- [ ] Tag wrap: wrap the selection in a new element
- [ ] Tag unwrap: remove the surrounding tag, keep the inner content
- [ ] Auto-completion for tag and attribute names from the document vocabulary

### Session & Preferences (SES-*)
- [ ] Reopen the last file on launch (the path is already persisted)
- [ ] Per-file state: scroll position, cursor, bookmarks, word wrap
- [ ] Preferences dialog (Ctrl+,): font, indent style, colour theme, column
      markers, auto-close, recent file count, large-file save strategy
- [ ] Follow the system light/dark setting (manual toggle exists)
- [ ] Crash recovery: auto-save every 60 s, offer at next launch (REL-05)

### File I/O (FIO-*)
- [ ] Large-file save choice (≥ 500 MB): in-place patch vs full rewrite (FIO-07).
      Saving always rewrites the whole file today, which needs free space equal
      to the document size.
- [ ] Saving over the currently mapped file relies on `QSaveFile`'s rename
      leaving the old inode mapped; the mapping is not refreshed afterwards

---

## Tests

- [x] `tst_MmapBuffer`, `tst_PieceTable`, `tst_SparseLineIndex`,
      `tst_FormatEngine`, `tst_SearchEngine`, `tst_XmlScanner`, `tst_Encoding`
- [x] UI smoke tests driving real key and mouse events (`tst_ViewportEditing`)
- [x] Round-trip test: open → beautify → save → reopen byte-identical
- [x] Clean under ASan + UBSan with leak detection
- [ ] Memory assertions in CI: RSS ≤ 80 MB for a 2 GB file (verified manually
      at 54–71 MB, but not asserted by a test — needs a large fixture)
- [ ] Regression fixtures from 1 KB to 2 GB generated at test time
- [ ] Fuzz targets for `IncrementalHighlighter` and `VirtualTreeModel`
- [ ] Performance benchmarks with CI regression alerts (> 10 % tolerance)
- [ ] `tst_MainWindow`: save, find, beautify and reload driven through the UI

---

## Packaging & Distribution

- [x] AppImage: `linuxdeploy` + Qt plugin wired into the CI Linux job
- [x] Application icon (`packaging/linux/loxe.png`), required by `linuxdeploy`
      because `loxe.desktop` declares `Icon=loxe`
- [ ] macOS: `create-dmg` instead of raw `hdiutil`, plus code-signing and
      notarisation (the DMG is currently unsigned, so Gatekeeper will block it)
- [ ] Linux `.desktop` MIME type registration (`application/xml`, `text/xml`)
- [ ] AppStream metadata for software-center discovery
- [ ] macOS `QFileOpenEvent` handling (Finder open, Dock drag)
- [ ] A macOS icon: `Info.plist` has no `CFBundleIconFile`, so the bundle uses
      the generic application icon
- [ ] Smoke-test the produced AppImage in CI (it is built and uploaded, but
      never launched, so a broken bundle would still go green)
- [ ] There is no README. It should mention that Qt 6.5+ needs `libxcb-cursor0`
      at runtime on Linux, which is the dependency that broke CI packaging.
