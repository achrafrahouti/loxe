#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>
#include <QtTest>

#include <fstream>
#include <string>

#include "engine/MmapBuffer.h"
#include "engine/PieceTable.h"

// True when the binary is built with AddressSanitizer or UBSan, which slow
// execution by an order of magnitude. Throughput and latency assertions are
// meaningless there and must be skipped rather than reported as failures.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#  define LOXE_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define LOXE_SANITIZED 1
#  endif
#endif

#if defined(LOXE_SANITIZED)
#  define LOXE_SKIP_IF_SANITIZED() \
      QSKIP("performance assertion is not meaningful under sanitizers")
#else
#  define LOXE_SKIP_IF_SANITIZED() do {} while (false)
#endif

// Shared fixtures for the engine tests.
namespace loxe_test {

// Writes `bytes` to a file inside `dir` and returns its path.
inline QString writeTempFile(const QTemporaryDir& dir, const QString& name,
                             const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(bytes);
    f.close();
    return path;
}

// Reads the whole document out of a PieceTable as a byte string.
inline std::string dump(const PieceTable& pt)
{
    return pt.read(0, pt.length());
}

// Concatenates every chunk the iterator yields — should equal dump().
inline std::string dumpViaIterator(const PieceTable& pt)
{
    std::string out;
    auto it = pt.begin();
    while (!it.atEnd()) {
        const std::string_view chunk = it.nextChunk();
        out.append(chunk.data(), chunk.size());
    }
    return out;
}

// Resident set size in KB, or -1 when unavailable. Linux only.
inline long currentRssKb()
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long value = -1;
            status >> value;
            return value;
        }
        std::getline(status, key);
    }
#endif
    return -1;
}

} // namespace loxe_test
