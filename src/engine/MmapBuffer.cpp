#include "MmapBuffer.h"

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
    : m_data(o.m_data), m_size(o.m_size), m_fd(o.m_fd), m_usePread(o.m_usePread)
{
    o.m_data = nullptr; o.m_size = 0; o.m_fd = -1;
}

MmapBuffer& MmapBuffer::operator=(MmapBuffer&& o) noexcept
{
    if (this != &o) { close(); new (this) MmapBuffer(std::move(o)); }
    return *this;
}

bool MmapBuffer::open(const char* path)
{
    close();
    return openMmap(path) || openPread(path);
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
}

std::string_view MmapBuffer::slice(off_t offset, size_t len) const
{
    if (!m_usePread)
        return {static_cast<const char*>(m_data) + offset, len};

    m_preadBuf.resize(len);
#if defined(__linux__) || defined(__APPLE__)
    ::pread(m_fd, m_preadBuf.data(), len, offset);
#endif
    return {m_preadBuf.data(), len};
}

void MmapBuffer::adviseSequential()
{
#if defined(__linux__) || defined(__APPLE__)
    if (!m_usePread && m_data)
        ::madvise(m_data, static_cast<size_t>(m_size), MADV_SEQUENTIAL);
#endif
}

void MmapBuffer::adviseRandom()
{
#if defined(__linux__) || defined(__APPLE__)
    if (!m_usePread && m_data)
        ::madvise(m_data, static_cast<size_t>(m_size), MADV_RANDOM);
#endif
}

bool MmapBuffer::remap()
{
    // TODO: store path at open() time; munmap + reopen
    return false;
}

bool MmapBuffer::openMmap(const char* path)
{
#if defined(__linux__) || defined(__APPLE__)
    m_fd = ::open(path, O_RDONLY);
    if (m_fd < 0) return false;

    struct stat st{};
    if (::fstat(m_fd, &st) < 0) { ::close(m_fd); m_fd = -1; return false; }
    m_size = static_cast<uint64_t>(st.st_size);

    void* p = ::mmap(nullptr, static_cast<size_t>(m_size), PROT_READ, MAP_PRIVATE, m_fd, 0);
    if (p == MAP_FAILED) return false;

    m_data     = p;
    m_usePread = false;
    return true;
#else
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
        m_size = static_cast<uint64_t>(st.st_size);
    }
    m_usePread = true;
    return true;
#else
    return false;
#endif
}
