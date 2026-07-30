#pragma once

#include <string>
#include <string_view>

class PieceTable;

// Encoding detection per ENC-01: byte-order mark first, then the encoding
// pseudo-attribute of the XML declaration, then UTF-8 validity, else Latin-1.
namespace Encoding {

struct Info {
    std::string name     = "UTF-8";
    size_t      bomBytes = 0;     // length of the BOM, 0 if none
    bool        hasBom   = false;
};

// Inspects at most the first few KB of the document.
Info detect(const PieceTable& doc);

// True if `bytes` is a valid UTF-8 sequence (ignoring truncation at the end).
bool isValidUtf8(std::string_view bytes);

} // namespace Encoding
