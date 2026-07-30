#include "MmapBuffer.h"

#include <algorithm>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

MmapBuffer::~MmapBuffer()
{
    close();
}

MmapBuffer::MmapBuffer(MmapBuffer&& o) noexcept
    : m_data(o.m_data), m_size(o.m_size), m_fd(o.m_fd),
      m_usePread(o.m_usePread), m_path(std::move(o.m_path))
{
    o.m_data = nullptr; o.m_size = 0; o.m_fd = -1; o.m_usePread = false;
}

MmapBuffer& MmapBuffer::operator=(MmapBuffer&& o) noexcept
{
    if (this != &o) {
        close();
        m_data     = o.m_data;
        m_size     = o.m_size;
        m_fd       = o.m_fd;
        m_usePread = o.m_usePread;
        m_path     = std::move(o.m_path);
        o.m_data = nullptr; o.m_size = 0; o.m_fd = -1; o.m_usePread = false;
    }
    return *this;
}

bool MmapBuffer::open(const char* path)
{
    close();
    if (!path || !*path) return false;
    m_path = path;
    if (openMmap(path) || openPread(path)) return true;
    m_path.clear();
    return false;
}

void MmapBuffer::close()
{
#if defined(__linux__) || defined(__APPLE__)
    if (m_data && !m_usePread)
        ::munmap(m_data, static_cast<size_t>(m_size));
    if (m_fd >= 0)
        ::close(m_fd);
#endif
    m_data = nullptr; m_size = 0; m_fd = -1; m_usePread = false;
    m_path.clear();
}

std::string_view MmapBuffer::slice(uint64_t offset, size_t len) const
{
    if (m_fd < 0 || offset >= m_size || len == 0) return {};
    len = static_cast<size_t>(std::min<uint64_t>(len, m_size - offset));

    if (!m_usePread) {
        if (!m_data) return {};
        return {static_cast<const char*>(m_data) + offset, len};
    }

    // pread path: a thread-local scratch keeps concurrent readers from
    // clobbering each other's views (PieceTable permits multiple readers).
    static thread_local std::string scratch;
    len = std::min(len, kMaxPreadSlice);
    if (scratch.size() < len) scratch.resize(len);

#if defined(__linux__) || defined(__APPLE__)
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::pread(m_fd, scratch.data() + got, len - got,
                                  static_cast<off_t>(offset + got));
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    return {scratch.data(), got};
#else
    return {};
#endif
}

void MmapBuffer::adviseSequential()
{
#if defined(__linux__) || defined(__APPLE__)
    if (!m_usePread && m_data && m_size > 0)
        ::madvise(m_data, static_cast<size_t>(m_size), MADV_SEQUENTIAL);
#endif
}

void MmapBuffer::adviseRandom()
{
#if defined(__linux__) || defined(__APPLE__)
    if (!m_usePread && m_data && m_size > 0)
        ::madvise(m_data, static_cast<size_t>(m_size), MADV_RANDOM);
#endif
}

void MmapBuffer::adviseDontNeed(uint64_t offset, uint64_t len) const
{
#if defined(__linux__) || defined(__APPLE__)
    if (m_usePread || !m_data || offset >= m_size) return;
    len = std::min<uint64_t>(len, m_size - offset);

    // madvise() requires a page-aligned start; round up so we never drop pages
    // the caller may still be reading from.
    const auto pageSize = static_cast<uint64_t>(::getpagesize());
    const uint64_t start = (offset + pageSize - 1) & ~(pageSize - 1);
    if (start >= offset + len) return;

    const uint64_t end = offset + len;
    ::madvise(static_cast<char*>(m_data) + start,
              static_cast<size_t>(end - start), MADV_DONTNEED);
#else
    (void)offset; (void)len;
#endif
}

bool MmapBuffer::remap()
{
    if (m_path.empty()) return false;
    const std::string saved = m_path; // close() clears m_path
    return open(saved.c_str());
}

bool MmapBuffer::openMmap(const char* path)
{
#if defined(__linux__) || defined(__APPLE__)
    m_fd = ::open(path, O_RDONLY);
    if (m_fd < 0) return false;

    struct stat st{};
    if (::fstat(m_fd, &st) < 0) { ::close(m_fd); m_fd = -1; return false; }
    if (!S_ISREG(st.st_mode))   { ::close(m_fd); m_fd = -1; return false; }
    m_size = static_cast<uint64_t>(st.st_size);

    // mmap() of length 0 always fails; an empty file is legitimately empty.
    if (m_size == 0) {
        m_data     = nullptr;
        m_usePread = false;
        return true;
    }

    void* p = ::mmap(nullptr, static_cast<size_t>(m_size), PROT_READ, MAP_PRIVATE, m_fd, 0);
    if (p == MAP_FAILED) return false; // m_fd stays open for the pread fallback

    m_data     = p;
    m_usePread = false;
    return true;
#else
    (void)path;
    return false;
#endif
}

bool MmapBuffer::openPread(const char* path)
{
#if defined(__linux__) || defined(__APPLE__)
    if (m_fd < 0) {
        m_fd = ::open(path, O_RDONLY);
        if (m_fd < 0) return false;

        struct stat st{};
        if (::fstat(m_fd, &st) < 0) { ::close(m_fd); m_fd = -1; return false; }
        if (!S_ISREG(st.st_mode))   { ::close(m_fd); m_fd = -1; return false; }
        m_size = static_cast<uint64_t>(st.st_size);
    }
    m_usePread = true;
    return true;
#else
    (void)path;
    return false;
#endif
}
