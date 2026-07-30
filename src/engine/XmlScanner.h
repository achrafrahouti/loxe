#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

class PieceTable;

// A single markup or text node reported by XmlScanner.
struct XmlNode {
    enum class Kind {
        Text,
        StartTag,             // <a ...>
        EndTag,               // </a>
        EmptyTag,             // <a .../>
        Comment,              // <!-- ... -->
        Cdata,                // <![CDATA[ ... ]]>
        ProcessingInstruction, // <? ... ?>
        Doctype,              // <!DOCTYPE ... >
    };

    Kind             kind   = Kind::Text;
    uint64_t         offset = 0;  // byte offset of the node's first byte
    std::string_view raw;         // the node's full text; valid for this call only
    std::string_view name;        // element name, for Start/End/Empty tags only
};

// Streaming XML tokeniser over a PieceTable.
//
// The whole point is that it never holds more than one node in memory, so it
// works on a 2 GB document within the resident-memory budget. It is deliberately
// a scanner rather than a validating parser: no DTD handling, no namespace
// resolution, no well-formedness enforcement. CMarkup covers validation, but it
// requires the entire document in memory, which rules it out for the tree.
//
// This is the shared tokeniser behind both FormatEngine and VirtualTreeModel.
class XmlScanner {
public:
    // Return false to stop the scan early.
    using NodeFn = std::function<bool(const XmlNode&)>;

    // Text nodes longer than this are split across consecutive Text callbacks
    // so a document that is one huge text node still streams in bounded memory.
    static constexpr size_t kMaxTextNode = 256 * 1024;

    // Scans [from, to) of `doc`. Returns false if the callback stopped the scan
    // or the cancellation flag was raised.
    static bool scan(const PieceTable& doc, uint64_t from, uint64_t to,
                     const NodeFn& cb, const std::atomic<bool>* cancelled = nullptr);

    static bool scanAll(const PieceTable& doc, const NodeFn& cb,
                        const std::atomic<bool>* cancelled = nullptr);

    // Extracts the value of `attr` from a start-tag's raw text, or an empty
    // view if absent. Returned view points into `rawTag`.
    static std::string_view attributeValue(std::string_view rawTag, std::string_view attr);

    // Extracts the first attribute of a start tag. Returns false if it has none.
    static bool firstAttribute(std::string_view rawTag,
                               std::string_view* name, std::string_view* value);
};
