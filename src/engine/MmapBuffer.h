#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Zero-copy read-only view over a file via mmap(PROT_READ, MAP_PRIVATE).
// Automatically falls back to pread() if mmap() fails (e.g. low VA space).
// Not copyable; movable.
class MmapBuffer {
public:
    // Largest span a single pread() slice will materialise. Callers that need
    // more must loop; slice() is documented to return short views.
    static constexpr size_t kMaxPreadSlice = 1024 * 1024;

    MmapBuffer() = default;
    ~MmapBuffer();

    MmapBuffer(const MmapBuffer&) = delete;
    MmapBuffer& operator=(const MmapBuffer&) = delete;
    MmapBuffer(MmapBuffer&&) noexcept;
    MmapBuffer& operator=(MmapBuffer&&) noexcept;

    bool open(const char* path);
    void close();

    bool               isOpen()    const { return m_fd >= 0; }
    uint64_t           size()      const { return m_size; }
    bool               usesPread() const { return m_usePread; }
    const std::string& path()      const { return m_path; }

    // Returns a view of at most len bytes starting at offset, clamped to the
    // end of the file. May be SHORTER than len — on the pread path a slice is
    // capped at kMaxPreadSlice — so callers must loop until they have enough.
    //
    // On the mmap path the view aliases the mapping and stays valid until
    // close(). On the pread path it is backed by a thread-local scratch buffer
    // and is invalidated by the next slice() call on the same thread.
    std::string_view slice(uint64_t offset, size_t len) const;

    // Hint the kernel about expected access pattern.
    void adviseSequential();
    void adviseRandom();

    // Tell the kernel the pages covering [offset, offset+len) are no longer
    // needed. Used by sequential scans to keep resident RSS flat on huge files:
    // mapped pages count toward RSS while resident, so a full 2 GB scan would
    // otherwise blow the 80 MB budget.
    //
    // Const because it changes no logical state: the mapping is PROT_READ, so
    // there are never private modifications for MADV_DONTNEED to discard, and a
    // later read simply faults the page back in from the file.
    void adviseDontNeed(uint64_t offset, uint64_t len) const;

    // Re-open and re-map after an external file modification.
    bool remap();

private:
    bool openMmap(const char* path);
    bool openPread(const char* path);

    void*       m_data     = nullptr;
    uint64_t    m_size     = 0;
    int         m_fd       = -1;
    bool        m_usePread = false;
    std::string m_path;
};
