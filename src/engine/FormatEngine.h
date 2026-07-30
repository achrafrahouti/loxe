#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class PieceTable;

// Streaming beautify / minify processor.
//
// Reads input via PieceTable::Iterator and pushes output through a sink in
// bounded blocks, so the full document is never materialised (peak output
// buffer ≤ kOutputBufferSize). The tokeniser is a hand-written streaming XML
// scanner: CMarkup's tree API would require holding the whole document in
// memory, which the 2 GB / 80 MB budget rules out.
class FormatEngine {
public:
    // Output is handed to the sink in blocks no larger than this.
    static constexpr size_t kOutputBufferSize = 8 * 1024 * 1024; // 8 MB

    enum class Mode { Beautify, Minify };

    enum class IndentStyle { Spaces, Tabs };

    struct Options {
        Mode        mode        = Mode::Beautify;
        IndentStyle indentStyle = IndentStyle::Spaces;
        int         indentWidth = 2;
        // Line ending emitted between nodes in Beautify mode.
        std::string eol = "\n";
    };

    // Return true to cancel; called periodically during processing.
    using CancelFn   = std::function<bool()>;
    // Called with [0, 100] as processing progresses.
    using ProgressFn = std::function<void(int percent)>;
    // Receives output blocks in order. Return false to abort.
    using SinkFn     = std::function<bool(std::string_view block)>;

    // Streaming primitive. Returns false if cancelled or if the sink aborted.
    bool formatToSink(const PieceTable& src,
                      const Options&    opts,
                      const SinkFn&     sink,
                      ProgressFn        progress  = {},
                      CancelFn          cancelled = {});

    // Convenience wrapper: returns the formatted document as a new PieceTable
    // backed only by an ADD buffer, or nullptr if cancelled.
    // The result is held entirely in memory, so callers should prefer
    // formatToSink() for very large documents.
    std::unique_ptr<PieceTable> format(
        const PieceTable& src,
        const Options&    opts,
        ProgressFn        progress  = {},
        CancelFn          cancelled = {});
};
