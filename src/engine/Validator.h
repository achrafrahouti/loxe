#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class PieceTable;

// One parser complaint, positioned in the document.
struct XmlDiagnostic {
    int         line   = 0;  // 1-based; 0 when the parser gave no position
    int         column = 0;  // 1-based
    std::string message;     // trailing newline stripped
    bool        fatal  = false;
};

struct ValidationResult {
    bool                       wellFormed = false;
    bool                       cancelled  = false;
    std::vector<XmlDiagnostic> diagnostics;
    // True when diagnostics were dropped because the cap was reached.
    bool                       truncated  = false;

    // Convenience: the first fatal diagnostic, or the first of any kind.
    const XmlDiagnostic* primary() const;
};

// Well-formedness checking via libxml2's streaming xmlTextReader.
//
// The document is fed to libxml2 through a pull callback reading the PieceTable
// in bounded chunks, so a 2 GB file is checked without ever being materialised
// — which is why there is no size cap here. Runs on a background thread and
// honours a cancellation flag between reads.
class Validator {
public:
    // Diagnostics beyond this are dropped; a malformed document can otherwise
    // produce one error per line.
    static constexpr size_t kMaxDiagnostics = 100;

    // Bytes handed to libxml2 per pull.
    static constexpr size_t kChunkSize = 64 * 1024;

    static ValidationResult validate(const PieceTable&        doc,
                                     const std::atomic<bool>* cancelled = nullptr);
};
