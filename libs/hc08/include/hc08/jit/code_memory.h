// CodeMemory owns one region of memory for generated code and keeps it executable but not writable, except while a
// WritableScope is open. How "not writable" is achieved depends on the platform:
//
//   macOS arm64    mmap(MAP_JIT); pthread_jit_write_protect_np flips the calling thread between writing and
//                  executing; sys_icache_invalidate when a scope closes.
//   Linux          one memfd mapped twice: read+execute where the code runs, and read+write at another address
//                  that only a scope hands out (executable_address() is where bytes written there run). No protection
//                  ever changes, so installing code costs no system call. AArch64 also flushes the instruction cache
//                  when a scope closes. Where memfd_create fails, as macOS x86-64:
//   macOS x86-64   (also under Rosetta 2): the pages a scope allocates in are mprotect()ed read+write until it
//                  closes, and read+execute otherwise (only those pages: under Rosetta a protection change makes the
//                  translations of the pages it covers go).
//
// The only way to obtain writable bytes is WritableScope::allocate, and the code is never writable at the addresses it
// runs from (with two mappings, the writable bytes are elsewhere; otherwise they stop being writable when the scope
// ends), so generated code cannot be modified by accident. A CodeMemory is used from one thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace sim::jit {

enum class AllocationError {
    kOutOfCodeMemory,
};

class CodeMemory {
   public:
    // Code starts on this boundary, the strictest alignment either backend wants for a function entry.
    static constexpr size_t kCodeAlignment = 16;

    // Maps at least `capacity_bytes` (rounded up to whole pages). Throws JitError if the host refuses.
    explicit CodeMemory(size_t capacity_bytes);
    ~CodeMemory();

    CodeMemory(const CodeMemory&) = delete;
    CodeMemory& operator=(const CodeMemory&) = delete;

    // Writable access to the region for the lifetime of the object; at most one scope may be open at a time.
    class WritableScope {
       public:
        ~WritableScope();

        WritableScope(const WritableScope&) = delete;
        WritableScope& operator=(const WritableScope&) = delete;

        // Reserves `size_bytes` at the next kCodeAlignment boundary. The span is writable until this scope ends; the
        // code runs from executable_address(span.data()).
        std::expected<std::span<std::byte>, AllocationError> allocate(size_t size_bytes);

        // The same from the cold end: the region's top, growing down, for code that seldom runs (a host keeps it
        // apart from the code that does, which then lies closer together). The two ends meet when the memory is full.
        std::expected<std::span<std::byte>, AllocationError> allocate_cold(size_t size_bytes);

       private:
        friend class CodeMemory;
        explicit WritableScope(CodeMemory& memory);

        // Makes the pages of [begin, end) writable as well (per-page protection only).
        void extend_writable_range(size_t begin, size_t end);
        void extend_cold_writable_range(size_t begin, size_t end);

        CodeMemory& memory_;
        size_t first_offset_;
        size_t first_cold_offset_;
        // the pages made writable so far (per-page protection only): [begin, end) as offsets, at each end
        size_t writable_begin_ = 0;
        size_t writable_end_ = 0;
        size_t cold_writable_begin_ = 0;
        size_t cold_writable_end_ = 0;
    };

    // Throws JitError if a scope is already open or the protection cannot be changed.
    [[nodiscard]] WritableScope make_writable();

    // The memory is mapped twice: patch_word is a plain store (no protection change).
    bool mapped_twice() const {
        return mapped_twice_;
    }

    // Where the bytes a scope handed out at `writable` run from (the same address with one mapping).
    std::byte* executable_address(std::byte* writable) const {
        return base_ + (writable - writable_base_);
    }

    // Replaces the instruction word at `address` (inside an allocation of this memory, where it runs) and makes it
    // visible to execution. Not while a scope is open.
    void patch_word(uint32_t* address, uint32_t word);

    // Many patch_word calls in a row (resetting a drop's links): with the per-thread write toggle the region stays
    // writable for the scope's lifetime instead of being toggled per word. Not with a WritableScope open.
    class PatchScope {
       public:
        ~PatchScope();

        PatchScope(const PatchScope&) = delete;
        PatchScope& operator=(const PatchScope&) = delete;

       private:
        friend class CodeMemory;
        explicit PatchScope(CodeMemory& memory);

        CodeMemory& memory_;
    };

    [[nodiscard]] PatchScope begin_patching();

    // Forgets every allocation; all function pointers handed out so far become invalid. This is the first, blunt way
    // the JIT invalidates translated code. Throws JitError if a scope is open.
    void reset();

    size_t capacity_bytes() const {
        return capacity_bytes_;
    }
    size_t used_bytes() const {
        return used_bytes_;
    }
    // (from the top: allocate_cold)
    size_t cold_used_bytes() const {
        return cold_used_bytes_;
    }
    const std::byte* base() const {
        return base_;
    }

   private:
    // Makes [begin, end) (whole pages) writable or executable: the calling thread's view of the whole region with the
    // per-thread toggle, those pages' protection otherwise.
    void set_writable(bool writable, size_t begin, size_t end);

    std::byte* base_ = nullptr;           // where the code runs
    std::byte* writable_base_ = nullptr;  // where it is written: base_ unless the memory is mapped twice
    bool mapped_twice_ = false;
    size_t page_bytes_ = 0;
    size_t capacity_bytes_ = 0;
    size_t used_bytes_ = 0;
    size_t cold_used_bytes_ = 0;
    bool scope_open_ = false;
    bool patching_ = false;  // a PatchScope is open
};

}  // namespace sim::jit
