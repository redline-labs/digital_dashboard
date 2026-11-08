#include "hc08/jit/host_probe.h"

#include <cstring>

#include <asmjit/core.h>
#if defined(__aarch64__)
#include <asmjit/a64.h>
#elif defined(__x86_64__)
#include <asmjit/x86.h>
#endif

#include "hc08/jit/code_memory.h"
#include "hc08/jit/jit_error.h"

namespace sim::jit {

std::string host_architecture() {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "none";
#endif
}

uint64_t run_add_probe(uint64_t x, uint32_t addend) {
#if defined(__aarch64__) || defined(__x86_64__)
    asmjit::Environment env = asmjit::Environment::host();
    asmjit::CodeHolder code;
    code.init(env);

#if defined(__aarch64__)
    asmjit::a64::Assembler a(&code);
    a.mov(asmjit::a64::x1, uint64_t(addend));
    a.add(asmjit::a64::x0, asmjit::a64::x0, asmjit::a64::x1);
    a.ret(asmjit::a64::x30);
#else
    asmjit::x86::Assembler a(&code);
#if defined(_WIN32)
    a.mov(asmjit::x86::rax, asmjit::x86::rcx);
#else
    a.mov(asmjit::x86::rax, asmjit::x86::rdi);
#endif
    a.add(asmjit::x86::rax, int32_t(addend));
    a.ret();
#endif

    code.flatten();
    const size_t size = code.code_size();

    CodeMemory memory(1 << 16);
    std::byte* entry = nullptr;
    {
        CodeMemory::WritableScope scope = memory.make_writable();
        auto span = scope.allocate(size);
        if (!span) {
            throw JitError("probe: code memory full");
        }
        if (code.relocate_to_base(uint64_t(reinterpret_cast<uintptr_t>(memory.executable_address(span->data())))) !=
            asmjit::Error::kOk) {
            throw JitError("probe: relocation failed");
        }
        code.copy_flattened_data(span->data(), span->size());
        entry = memory.executable_address(span->data());
    }

    using Fn = uint64_t (*)(uint64_t);
    Fn fn;
    std::memcpy(&fn, &entry, sizeof(fn));
    return fn(x);
#else
    (void)x;
    (void)addend;
    throw JitError("no JIT assembler for this host");
#endif
}

}  // namespace sim::jit
