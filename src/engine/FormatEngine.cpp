#include "FormatEngine.h"
#include "PieceTable.h"

std::unique_ptr<PieceTable> FormatEngine::format(
    const PieceTable& src,
    const Options&    opts,
    ProgressFn        progress,
    CancelFn          cancelled)
{
    // TODO: implement streaming beautify/minify via CMarkup
    //
    // Outline:
    //  1. Create an output PieceTable backed by a null MmapBuffer (ADD-only).
    //  2. Iterate src via PieceTable::Iterator, feeding chunks into CMarkup.
    //  3. For each CMarkup node type (MNT_ELEMENT, MNT_TEXT, MNT_CDATA_SECTION,
    //     MNT_COMMENT, MNT_PROCESSING_INSTRUCTION, MNT_DOCUMENT_TYPE):
    //     - Beautify: emit indented tag, accumulate into kOutputBufferSize buffer,
    //       flush via output PieceTable::insert when buffer is full.
    //     - Minify: strip whitespace-only text nodes.
    //  4. Report progress every ~100 ms.
    //  5. Return nullptr if cancelled() returns true.

    (void)src; (void)opts; (void)progress; (void)cancelled;
    return nullptr;
}
