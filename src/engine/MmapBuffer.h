#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>

// Zero-copy read-only view over a file via mmap(PROT_READ, MAP_PRIVATE).
// Automatically falls back to pread() if mmap() fails (e.g. low VA space).
// Not copyable; movable.
class MmapBuffer {
public:
    MmapBuffer() = default;
    ~MmapBuffer();

    MmapBuffer(const MmapBuffer&) = delete;
    MmapBuffer& operator=(const MmapBuffer&) = delete;
    MmapBuffer(MmapBuffer&&) noexcept;
    MmapBuffer& operator=(MmapBuffer&&) noexcept;

    bool open(const char* path);
    void close();

    bool     isOpen() const { return m_fd >= 0; }
    uint64_t size()   const { return m_size; }
    bool     usesPread() const { return m_usePread; }

    // Returns a zero-copy view for the mmap path.
    // On the pread path the view is backed by an internal scratch buffer;
    // do not hold views across calls on that path.
    std::string_view slice(off_t offset, size_t len) const;

    // Hint the kernel about expected access pattern.
    void adviseSequential();
    void adviseRandom();

    // Remap after an external file modification (requires reopen).
    bool remap();

private:
    bool openMmap(const char* path);
    bool openPread(const char* path);

    void*    m_data     = nullptr;
    uint64_t m_size     = 0;
    int      m_fd       = -1;
    bool     m_usePread = false;

    mutable std::string m_preadBuf; // scratch buffer — pread path only
};
