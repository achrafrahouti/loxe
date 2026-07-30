#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

class PieceTable;

// Declared at namespace scope: default member initializers of a *nested* class
// cannot be used in a default argument of its enclosing class.
struct SearchOptions {
    bool caseSensitive = true;
    bool wrapAround    = true;
};

// Plain-text and case-insensitive search over a PieceTable.
//
// Scans in overlapping windows so a match spanning a chunk boundary is still
// found, and delegates the inner loop to memmem() where available (glibc's is
// SIMD-accelerated, which is what the ≥ 500 MB/s target relies on).
// Regex search lives in the UI layer, which has QRegularExpression.
class SearchEngine {
public:
    static constexpr uint64_t kNotFound = UINT64_MAX;
    static constexpr size_t   kWindow   = 1024 * 1024;

    using Options = SearchOptions;

    // Receives each match's byte offset in ascending order. Return false to stop.
    using MatchFn = std::function<bool(uint64_t offset)>;

    // Every non-overlapping match at or after `from`, in one pass over the
    // document. Callers that want more than one hit must use this rather than
    // re-entering findForward() from the previous match: each such call rescans
    // a whole window, which turns "all matches" into a quadratic operation.
    // Ignores opts.wrapAround.
    static void forEachMatch(const PieceTable& doc, std::string_view needle,
                             uint64_t from, const Options& opts,
                             const std::atomic<bool>* cancelled,
                             const MatchFn& cb);

    // First match at or after `from`. Returns kNotFound if there is none.
    static uint64_t findForward(const PieceTable& doc, std::string_view needle,
                                uint64_t from, const Options& opts = Options{},
                                const std::atomic<bool>* cancelled = nullptr);

    // Last match strictly before `before`. Returns kNotFound if there is none.
    static uint64_t findBackward(const PieceTable& doc, std::string_view needle,
                                 uint64_t before, const Options& opts = Options{},
                                 const std::atomic<bool>* cancelled = nullptr);

    // Number of matches in the whole document (non-overlapping).
    static uint64_t countAll(const PieceTable& doc, std::string_view needle,
                             const Options& opts = Options{},
                             const std::atomic<bool>* cancelled = nullptr);
};
