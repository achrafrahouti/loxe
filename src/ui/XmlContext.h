#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <cstdint>

class PieceTable;

// The chain of open elements enclosing a byte offset, plus the attributes of
// the innermost one. Feeds the breadcrumb bar and the attribute panel.
struct XmlContextInfo {
    QStringList                   ancestors;   // outermost → innermost element names
    QString                       tagName;     // innermost element (last ancestor)
    QList<QPair<QString, QString>> attributes; // attributes of the innermost element
    bool                          truncated = false; // scan budget was exhausted
};

namespace XmlContext {

// Bytes scanned either side of the cursor before giving up. Bounded so that
// moving the cursor in a 2 GB document stays interactive; when the budget runs
// out the result is marked truncated and the breadcrumb shows a leading "…".
constexpr uint64_t kDefaultBudget = 1024 * 1024;

// Recovers the chain of elements enclosing `offset`.
//
// Works *forwards* from the cursor: an element encloses it exactly when that
// element's end tag appears with no matching start tag in between, so the
// unmatched end tags are the ancestors. Tokenising with XmlScanner means a '<'
// inside a comment, CDATA section or attribute value is never mistaken for a
// tag, and an unclosed element before the cursor (an HTML-style <br>) is not
// reported as an ancestor — both of which a backward scan gets wrong.
XmlContextInfo contextAt(const PieceTable& doc, uint64_t offset,
                         uint64_t budget = kDefaultBudget);

} // namespace XmlContext
