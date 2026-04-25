#pragma once

#include <cstddef>
#include <functional>
#include <memory>

class PieceTable;

// Streaming beautify / minify processor.
// Reads input via PieceTable::Iterator; writes a new PieceTable.
// Never materialises the full document in memory (output buffer ≤ 8 MB).
// The entire operation becomes a single undo step in the returned PieceTable.
class FormatEngine {
public:
    enum class Mode { Beautify, Minify };

    enum class IndentStyle { Spaces, Tabs };

    struct Options {
        Mode        mode        = Mode::Beautify;
        IndentStyle indentStyle = IndentStyle::Spaces;
        int         indentWidth = 2;
    };

    // Return true to cancel; called periodically during processing.
    using CancelFn   = std::function<bool()>;
    // Called with [0, 100] as processing progresses.
    using ProgressFn = std::function<void(int percent)>;

    // Returns the formatted document as a new PieceTable backed by an ADD buffer,
    // or nullptr if cancelled or if the input is not parseable XML.
    std::unique_ptr<PieceTable> format(
        const PieceTable& src,
        const Options&    opts,
        ProgressFn        progress  = {},
        CancelFn          cancelled = {});

private:
    static constexpr size_t kOutputBufferSize = 8 * 1024 * 1024; // 8 MB
};
