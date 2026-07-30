# loxe

`loxe` is a native desktop XML editor for Linux and macOS.

## Why this project exists

`loxe` exists to provide a modern, native successor to the Windows-only *foxe* editor, with a strong focus on very large XML files.

The core goal is to keep editing responsive and memory usage low even for multi-GB documents by streaming data through a piece-table based engine instead of loading whole files into RAM.

## Requirements

- C++17 compiler
- CMake 3.24+
- Qt 6.4+ (Qt 6 only)
- libxml2 2.9+

### Linux (Debian/Ubuntu) packages

```bash
sudo apt-get update
sudo apt-get install -y qt6-base-dev libxml2-dev
```

### macOS

- Install Qt 6 (for example via Qt Online Installer or Homebrew setup you use locally).
- libxml2 is available from the macOS SDK.

## Build

### Release build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Debug build (ASan + UBSan enabled by default)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

```bash
./build/src/loxe
```

Open a file directly:

```bash
./build/src/loxe /path/to/file.xml
```

## Test

Run the full test suite:

```bash
cmake --build build --target test
```

Or with CTest output:

```bash
ctest --test-dir build --output-on-failure
```

Run one test binary:

```bash
./build/tests/tst_PieceTable -v2
```

## Notes

- Linux distribution uses AppImage packaging.
- macOS build and tests are supported; DMG packaging may be enabled/disabled depending on CI workflow status.

