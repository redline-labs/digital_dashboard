#include "hc08/jit/code_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "hc08/jit/jit_error.h"

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

namespace sim::jit {

namespace {

// Apple Silicon refuses runtime-generated code outside a MAP_JIT mapping, and flips write access per thread instead of
// per page; every other supported host changes the page protection.
#if defined(__APPLE__) && defined(__aarch64__)
constexpr bool kPerThreadWriteToggle = true;
#else
constexpr bool kPerThreadWriteToggle = false;
#endif

// Only AArch64 has an instruction cache that is not coherent with data writes; x86-64 needs nothing.
#if defined(__aarch64__)
constexpr bool kNeedsInstructionCacheFlush = true;
#else
constexpr bool kNeedsInstructionCacheFlush = false;
#endif

constexpr int kExecutableProtection = PROT_READ | PROT_EXEC;
constexpr int kWritableProtection = PROT_READ | PROT_WRITE;

std::string describe_errno(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

size_t round_up(size_t value, size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

void flush_instruction_cache(std::byte* begin, size_t size_bytes) {
    if constexpr (!kNeedsInstructionCacheFlush) {
        return;
    }

    if (size_bytes == 0) {
        return;
    }

#if defined(__APPLE__)
    sys_icache_invalidate(begin, size_bytes);
#else
    __builtin___clear_cache(reinterpret_cast<char*>(begin), reinterpret_cast<char*>(begin + size_bytes));
#endif
}

// The region as one memfd mapped twice, read+execute and read+write: false (nothing mapped) where the host has no
// memfd or refuses.
bool map_twice(size_t capacity_bytes, std::byte*& executable, std::byte*& writable) {
#if defined(__linux__)
    const int file = memfd_create("sim-jit-code", MFD_CLOEXEC);
    if (file < 0) {
        return false;
    }

    const bool sized = ftruncate(file, static_cast<off_t>(capacity_bytes)) == 0;
    void* execute_view = sized ? mmap(nullptr, capacity_bytes, kExecutableProtection, MAP_SHARED, file, 0) : MAP_FAILED;
    void* write_view = execute_view != MAP_FAILED
                           ? mmap(nullptr, capacity_bytes, kWritableProtection, MAP_SHARED, file, 0)
                           : MAP_FAILED;
    close(file);

    if (write_view == MAP_FAILED) {
        if (execute_view != MAP_FAILED) {
            munmap(execute_view, capacity_bytes);
        }

        return false;
    }

    executable = static_cast<std::byte*>(execute_view);
    writable = static_cast<std::byte*>(write_view);
    return true;
#else
    static_cast<void>(capacity_bytes);
    static_cast<void>(executable);
    static_cast<void>(writable);
    return false;
#endif
}

}  // namespace

CodeMemory::CodeMemory(size_t capacity_bytes) {
    page_bytes_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    capacity_bytes_ = round_up(capacity_bytes == 0 ? 1 : capacity_bytes, page_bytes_);

    // (SIM_JIT_CODE_MAPPING=once keeps one mapping where two are possible: the other hosts' way, to test it here)
    const char* const mapping_choice = std::getenv("SIM_JIT_CODE_MAPPING");
    const bool map_once = mapping_choice != nullptr && std::string_view(mapping_choice) == "once";
    if (!kPerThreadWriteToggle && !map_once && map_twice(capacity_bytes_, base_, writable_base_)) {
        mapped_twice_ = true;
        return;
    }

    int protection = kExecutableProtection;
    int flags = MAP_PRIVATE | MAP_ANON;
    if constexpr (kPerThreadWriteToggle) {
#if defined(__APPLE__)
        // A MAP_JIT mapping is created writable and executable at once; the per-thread toggle decides which applies.
        protection = kWritableProtection | PROT_EXEC;
        flags |= MAP_JIT;
#endif
    }

    void* mapping = mmap(nullptr, capacity_bytes_, protection, flags, -1, 0);
    if (mapping == MAP_FAILED) {
        throw JitError(describe_errno("mmap of code memory failed"));
    }

    base_ = static_cast<std::byte*>(mapping);
    writable_base_ = base_;

    if constexpr (kPerThreadWriteToggle) {
#if defined(__APPLE__)
        pthread_jit_write_protect_np(true);
#endif
    } else if (mprotect(base_, capacity_bytes_, kExecutableProtection) != 0) {
        const std::string message = describe_errno("mprotect of fresh code memory failed");
        munmap(base_, capacity_bytes_);
        throw JitError(message);
    }
}

CodeMemory::~CodeMemory() {
    munmap(base_, capacity_bytes_);
    if (mapped_twice_) {
        munmap(writable_base_, capacity_bytes_);
    }
}

void CodeMemory::set_writable(bool writable, size_t begin, size_t end) {
    if constexpr (kPerThreadWriteToggle) {
#if defined(__APPLE__)
        pthread_jit_write_protect_np(!writable);
#endif
        return;
    }

    if (begin == end || mapped_twice_) {
        return;
    }

    const int protection = writable ? kWritableProtection : kExecutableProtection;
    if (mprotect(base_ + begin, end - begin, protection) != 0) {
        throw JitError(describe_errno("mprotect of code memory failed"));
    }
}

CodeMemory::WritableScope CodeMemory::make_writable() {
    if (scope_open_) {
        throw JitError("a writable scope of this code memory is already open");
    }

    return WritableScope(*this);
}

void CodeMemory::patch_word(uint32_t* address, uint32_t word) {
    // (x86-64 patches a jump's rel32, which is not aligned and may cross into the next page)
    const size_t offset = static_cast<size_t>(reinterpret_cast<std::byte*>(address) - base_);
    const size_t page_begin = offset / page_bytes_ * page_bytes_;
    const size_t page_end = (offset + sizeof(word) + page_bytes_ - 1) / page_bytes_ * page_bytes_;

    // (inside a PatchScope the per-thread toggle is open already)
    const bool toggle = !(kPerThreadWriteToggle && patching_);

    if (toggle) {
        if constexpr (kPerThreadWriteToggle) {
            set_writable(true, 0, capacity_bytes_);
        } else {
            set_writable(true, page_begin, page_end);
        }
    }

    std::memcpy(writable_base_ + offset, &word, sizeof(word));

    if (toggle) {
        if constexpr (kPerThreadWriteToggle) {
            set_writable(false, 0, capacity_bytes_);
        } else {
            set_writable(false, page_begin, page_end);
        }
    }

    flush_instruction_cache(reinterpret_cast<std::byte*>(address), sizeof(uint32_t));
}

CodeMemory::PatchScope CodeMemory::begin_patching() {
    if (scope_open_ || patching_) {
        throw JitError("patching code memory with a scope open");
    }

    return PatchScope(*this);
}

CodeMemory::PatchScope::PatchScope(CodeMemory& memory) : memory_(memory) {
    memory_.patching_ = true;

    if constexpr (kPerThreadWriteToggle) {
        memory_.set_writable(true, 0, memory_.capacity_bytes_);
    }
}

CodeMemory::PatchScope::~PatchScope() {
    if constexpr (kPerThreadWriteToggle) {
        memory_.set_writable(false, 0, memory_.capacity_bytes_);
    }

    memory_.patching_ = false;
}

void CodeMemory::reset() {
    if (scope_open_) {
        throw JitError("code memory reset while a writable scope is open");
    }

    used_bytes_ = 0;
    cold_used_bytes_ = 0;
}

CodeMemory::WritableScope::WritableScope(CodeMemory& memory)
    : memory_(memory),
      first_offset_(memory.used_bytes_),
      first_cold_offset_(memory.capacity_bytes_ - memory.cold_used_bytes_) {
    if constexpr (kPerThreadWriteToggle) {
        memory_.set_writable(true, 0, memory_.capacity_bytes_);
    }

    memory_.scope_open_ = true;
}

CodeMemory::WritableScope::~WritableScope() {
    // Leaving the region writable would silently break the guarantee of this class, and a destructor cannot report
    // the failure by throwing, so a failure here ends the process with a message.
    try {
        if constexpr (kPerThreadWriteToggle) {
            memory_.set_writable(false, 0, memory_.capacity_bytes_);
        } else {
            memory_.set_writable(false, writable_begin_, writable_end_);
            memory_.set_writable(false, cold_writable_begin_, cold_writable_end_);
        }
    } catch (const JitError& error) {
        SPDLOG_CRITICAL("{}", error.what());
        std::abort();
    }

    memory_.scope_open_ = false;

    std::byte* written_begin = memory_.base_ + first_offset_;
    flush_instruction_cache(written_begin, memory_.used_bytes_ - first_offset_);

    const size_t cold_begin = memory_.capacity_bytes_ - memory_.cold_used_bytes_;
    flush_instruction_cache(memory_.base_ + cold_begin, first_cold_offset_ - cold_begin);
}

void CodeMemory::WritableScope::extend_writable_range(size_t begin, size_t end) {
    const size_t page_begin = begin / memory_.page_bytes_ * memory_.page_bytes_;
    const size_t page_end = round_up(end, memory_.page_bytes_);

    if (writable_begin_ == writable_end_) {
        writable_begin_ = page_begin;
        writable_end_ = page_begin;
    }

    // (allocations only move up, so the range only grows at its end)
    if (page_end > writable_end_) {
        memory_.set_writable(true, writable_end_, page_end);
        writable_end_ = page_end;
    }
}

// (the cold end's pages: a range that only grows at its start)
void CodeMemory::WritableScope::extend_cold_writable_range(size_t begin, size_t end) {
    const size_t page_begin = begin / memory_.page_bytes_ * memory_.page_bytes_;
    const size_t page_end = round_up(end, memory_.page_bytes_);

    if (cold_writable_begin_ == cold_writable_end_) {
        cold_writable_begin_ = page_end;
        cold_writable_end_ = page_end;
    }

    if (page_begin < cold_writable_begin_) {
        memory_.set_writable(true, page_begin, cold_writable_begin_);
        cold_writable_begin_ = page_begin;
    }
}

std::expected<std::span<std::byte>, AllocationError> CodeMemory::WritableScope::allocate_cold(size_t size_bytes) {
    const size_t top = memory_.capacity_bytes_ - memory_.cold_used_bytes_;
    if (size_bytes > top) {
        return std::unexpected(AllocationError::kOutOfCodeMemory);
    }

    const size_t start_offset = (top - size_bytes) / kCodeAlignment * kCodeAlignment;
    if (start_offset < memory_.used_bytes_) {
        return std::unexpected(AllocationError::kOutOfCodeMemory);
    }

    memory_.cold_used_bytes_ = memory_.capacity_bytes_ - start_offset;

    if constexpr (!kPerThreadWriteToggle) {
        extend_cold_writable_range(start_offset, start_offset + size_bytes);
    }

    return std::span<std::byte>(memory_.writable_base_ + start_offset, size_bytes);
}

std::expected<std::span<std::byte>, AllocationError> CodeMemory::WritableScope::allocate(size_t size_bytes) {
    const size_t start_offset = round_up(memory_.used_bytes_, kCodeAlignment);
    const size_t end_offset = start_offset + size_bytes;
    if (end_offset > memory_.capacity_bytes_ - memory_.cold_used_bytes_) {
        return std::unexpected(AllocationError::kOutOfCodeMemory);
    }

    memory_.used_bytes_ = end_offset;

    if constexpr (!kPerThreadWriteToggle) {
        extend_writable_range(start_offset, end_offset);
    }

    return std::span<std::byte>(memory_.writable_base_ + start_offset, size_bytes);
}

}  // namespace sim::jit
