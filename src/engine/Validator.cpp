#include "Validator.h"
#include "PieceTable.h"

#include <libxml/parser.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlreader.h>

#include <algorithm>
#include <cstring>

namespace {

// Release file pages behind the parser this often, so validating a 2 GB
// document does not leave the whole mapping resident.
constexpr uint64_t kReleaseEvery = 8ull * 1024 * 1024;

// State threaded through libxml2's I/O and error callbacks.
struct Context {
    const PieceTable*        doc       = nullptr;
    const std::atomic<bool>* cancelled = nullptr;
    uint64_t                 pos       = 0;
    uint64_t                 lastRelease = 0;
    ValidationResult*        result    = nullptr;
};

// libxml2 pulls input through this; returning 0 signals end of document.
int readCallback(void* opaque, char* buffer, int len)
{
    auto* ctx = static_cast<Context*>(opaque);
    if (!ctx || !ctx->doc || len <= 0) return 0;
    if (ctx->cancelled && ctx->cancelled->load()) return -1; // abort the parse

    const size_t got = ctx->doc->readInto(ctx->pos, buffer, static_cast<size_t>(len));
    ctx->pos += got;

    if (ctx->pos - ctx->lastRelease >= kReleaseEvery) {
        ctx->doc->releasePagesBefore(ctx->pos);
        ctx->lastRelease = ctx->pos;
    }
    return static_cast<int>(got);
}

int closeCallback(void*)
{
    return 0;
}

// libxml2 made the structured-error payload const in 2.12; older releases
// (Ubuntu 22.04 ships 2.9, as does the macOS SDK) pass it mutably.
#if LIBXML_VERSION >= 21200
using StructuredErrorPtr = const xmlError*;
#else
using StructuredErrorPtr = xmlError*;
#endif

void structuredError(void* opaque, StructuredErrorPtr error)
{
    auto* ctx = static_cast<Context*>(opaque);
    if (!ctx || !ctx->result || !error) return;

    // Warnings are noise for a live status indicator; keep errors and fatals.
    if (error->level == XML_ERR_WARNING) return;

    if (ctx->result->diagnostics.size() >= Validator::kMaxDiagnostics) {
        ctx->result->truncated = true;
        return;
    }

    XmlDiagnostic diag;
    diag.line   = error->line;
    diag.column = error->int2;
    diag.fatal  = (error->level == XML_ERR_FATAL);
    if (error->message) {
        diag.message = error->message;
        while (!diag.message.empty()
               && (diag.message.back() == '\n' || diag.message.back() == '\r'))
            diag.message.pop_back();
    }
    ctx->result->diagnostics.push_back(std::move(diag));
}

} // namespace

const XmlDiagnostic* ValidationResult::primary() const
{
    for (const auto& d : diagnostics)
        if (d.fatal) return &d;
    return diagnostics.empty() ? nullptr : &diagnostics.front();
}

ValidationResult Validator::validate(const PieceTable&        doc,
                                     const std::atomic<bool>* cancelled)
{
    ValidationResult result;

    // An empty document is not well-formed XML (no root element), but reporting
    // that on a blank editor is unhelpful noise — treat it as "nothing to say".
    if (doc.length() == 0) {
        result.wellFormed = true;
        return result;
    }

    Context ctx;
    ctx.doc       = &doc;
    ctx.cancelled = cancelled;
    ctx.result    = &result;

    // NONET + no DTDLOAD: never touch the network or the filesystem for an
    // external DTD. No NOENT: entity references are reported as nodes rather
    // than expanded, which sidesteps entity-expansion blowups.
    // HUGE lifts libxml2's 10 MB text-node and depth limits, which a large-file
    // editor would otherwise trip on legitimate documents.
    const int options = XML_PARSE_NONET | XML_PARSE_HUGE | XML_PARSE_NOBLANKS;

    xmlTextReaderPtr reader = xmlReaderForIO(
        readCallback, closeCallback, &ctx, /*URL=*/nullptr, /*encoding=*/nullptr, options);
    if (!reader) {
        result.wellFormed = false;
        result.diagnostics.push_back({0, 0, "Could not create the XML reader", true});
        return result;
    }

    xmlTextReaderSetStructuredErrorHandler(reader, structuredError, &ctx);

    int rc = 1;
    while ((rc = xmlTextReaderRead(reader)) == 1) {
        if (cancelled && cancelled->load()) {
            result.cancelled = true;
            break;
        }
    }

    if (rc < 0 && cancelled && cancelled->load())
        result.cancelled = true;

    xmlFreeTextReader(reader);

    if (result.cancelled) {
        result.wellFormed = false;
        return result;
    }

    // rc == 0 means the reader consumed the whole document successfully.
    result.wellFormed = (rc == 0) && result.diagnostics.empty();
    return result;
}
