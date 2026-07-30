#include "Encoding.h"
#include "PieceTable.h"

#include <algorithm>
#include <cctype>

namespace {

constexpr size_t kProbeBytes = 4096;

// Reads the value of the encoding pseudo-attribute from an XML declaration.
std::string declaredEncoding(std::string_view probe)
{
    const size_t declEnd = probe.find("?>");
    if (probe.compare(0, 5, "<?xml") != 0 || declEnd == std::string_view::npos) return {};

    const std::string_view decl = probe.substr(0, declEnd);
    const size_t at = decl.find("encoding");
    if (at == std::string_view::npos) return {};

    size_t i = at + 8;
    while (i < decl.size() && (decl[i] == ' ' || decl[i] == '\t')) ++i;
    if (i >= decl.size() || decl[i] != '=') return {};
    ++i;
    while (i < decl.size() && (decl[i] == ' ' || decl[i] == '\t')) ++i;
    if (i >= decl.size() || (decl[i] != '"' && decl[i] != '\'')) return {};

    const char quote = decl[i++];
    const size_t end = decl.find(quote, i);
    if (end == std::string_view::npos) return {};
    return std::string(decl.substr(i, end - i));
}

} // namespace

namespace Encoding {

bool isValidUtf8(std::string_view b)
{
    size_t i = 0;
    while (i < b.size()) {
        const auto c = static_cast<unsigned char>(b[i]);
        size_t extra = 0;
        if (c < 0x80)                    extra = 0;
        else if ((c & 0xE0) == 0xC0)     extra = 1;
        else if ((c & 0xF0) == 0xE0)     extra = 2;
        else if ((c & 0xF8) == 0xF0)     extra = 3;
        else return false;

        // A sequence cut off by the end of the probe is not a failure.
        if (i + extra >= b.size()) return true;
        for (size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(b[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

Info detect(const PieceTable& doc)
{
    Info info;
    const std::string probe = doc.read(0, kProbeBytes);
    if (probe.empty()) return info;

    const auto byteAt = [&](size_t i) {
        return i < probe.size() ? static_cast<unsigned char>(probe[i]) : 0u;
    };

    // 1. Byte-order mark.
    if (byteAt(0) == 0xEF && byteAt(1) == 0xBB && byteAt(2) == 0xBF)
        return {"UTF-8", 3, true};
    if (byteAt(0) == 0xFF && byteAt(1) == 0xFE)
        return {"UTF-16LE", 2, true};
    if (byteAt(0) == 0xFE && byteAt(1) == 0xFF)
        return {"UTF-16BE", 2, true};

    // 2. XML declaration.
    if (std::string declared = declaredEncoding(probe); !declared.empty()) {
        std::transform(declared.begin(), declared.end(), declared.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        info.name = declared;
        return info;
    }

    // 3. Fall back to UTF-8 when the bytes are valid, Latin-1 otherwise.
    info.name = isValidUtf8(probe) ? "UTF-8" : "ISO-8859-1";
    return info;
}

} // namespace Encoding
