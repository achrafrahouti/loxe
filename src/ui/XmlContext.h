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

// Bytes scanned backwards from the cursor before giving up. Bounded so that
// moving the cursor in a 2 GB document stays interactive; when the budget runs
// out the result is marked truncated and the breadcrumb shows a leading "…".
constexpr uint64_t kDefaultBudget = 4ull * 1024 * 1024;

// Walks backwards from `offset` matching end tags against start tags to
// recover the open-element chain. This is a scanner, not a parser: a '<' inside
// a comment or CDATA section can mislead it, which is acceptable for a
// navigation aid but is why it is not used for validation.
XmlContextInfo contextAt(const PieceTable& doc, uint64_t offset,
                         uint64_t budget = kDefaultBudget);

} // namespace XmlContext
