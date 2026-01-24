/**
 * =============================================================================
 * Quartz Copy-and-Patch JIT Compiler - Implementation
 * =============================================================================
 */

#include "jit.h"
#include "bytecode.h"
#include "runtime.h"
#include "types.h"

#include <cstring>
#include <chrono>
#include <algorithm>
#include <cstdio>

#ifdef QZ_JIT_DEBUG
#include <iostream>
#endif

// Include architecture-specific stencils
#if defined(__x86_64__) || defined(_M_X64)
#include "stencils_x86_64.h"
#define JIT_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "stencils_aarch64.h"
#define JIT_ARCH_AARCH64 1
#else
#error "Unsupported architecture for JIT"
#endif

// Platform-specific memory allocation
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qz::jit {

// =============================================================================
// CodeRegion Implementation
// =============================================================================

CodeRegion::CodeRegion() = default;

CodeRegion::~CodeRegion() {
    deallocate();
}

CodeRegion::CodeRegion(CodeRegion&& other) noexcept
    : code_(other.code_), capacity_(other.capacity_) {
    other.code_ = nullptr;
    other.capacity_ = 0;
}

CodeRegion& CodeRegion::operator=(CodeRegion&& other) noexcept {
    if (this != &other) {
        deallocate();
        code_ = other.code_;
        capacity_ = other.capacity_;
        other.code_ = nullptr;
        other.capacity_ = 0;
    }
    return *this;
}

bool CodeRegion::allocate(size_t size) {
    deallocate();
    
    // Round up to page size
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t pageSize = si.dwPageSize;
#else
    size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    
    size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);
    
#if defined(_WIN32)
    code_ = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, alignedSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    ));
#else
    code_ = static_cast<uint8_t*>(mmap(
        nullptr, alignedSize,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    ));
    if (code_ == MAP_FAILED) {
        code_ = nullptr;
    }
#endif
    
    if (code_) {
        capacity_ = alignedSize;
        return true;
    }
    return false;
}

void CodeRegion::deallocate() {
    if (code_) {
#if defined(_WIN32)
        VirtualFree(code_, 0, MEM_RELEASE);
#else
        munmap(code_, capacity_);
#endif
        code_ = nullptr;
        capacity_ = 0;
    }
}

bool CodeRegion::makeExecutable() {
    if (!code_) return false;
    
#if defined(_WIN32)
    DWORD oldProtect;
    return VirtualProtect(code_, capacity_, PAGE_EXECUTE_READ, &oldProtect) != 0;
#else
    return mprotect(code_, capacity_, PROT_READ | PROT_EXEC) == 0;
#endif
}

bool CodeRegion::makeWritable() {
    if (!code_) return false;
    
#if defined(_WIN32)
    DWORD oldProtect;
    return VirtualProtect(code_, capacity_, PAGE_READWRITE, &oldProtect) != 0;
#else
    return mprotect(code_, capacity_, PROT_READ | PROT_WRITE) == 0;
#endif
}

// =============================================================================
// Compiler Implementation
// =============================================================================

Compiler::Compiler(Runtime& runtime) : runtime_(runtime) {
    emitBuffer_.reserve(4096);
}

Compiler::~Compiler() = default;

void Compiler::resetEmitState() {
    emitBuffer_.clear();
    emitOffset_ = 0;
    labels_.clear();
}

void Compiler::emitByte(uint8_t b) {
    emitBuffer_.push_back(b);
    emitOffset_++;
}

void Compiler::emitBytes(const uint8_t* data, size_t len) {
    emitBuffer_.insert(emitBuffer_.end(), data, data + len);
    emitOffset_ += len;
}

void Compiler::emitU64(uint64_t v) {
    uint8_t bytes[8];
    std::memcpy(bytes, &v, 8);
    emitBytes(bytes, 8);
}

void Compiler::emitPrologue() {
#if JIT_ARCH_X86_64
    // System V AMD64 ABI: rdi=stack, rsi=locals, rdx=runtime
    // We save these to callee-saved registers so they persist across the function
    
    // push rbp
    emitByte(0x55);
    // mov rbp, rsp
    emitByte(0x48); emitByte(0x89); emitByte(0xe5);
    // push rbx (will hold stack ptr)
    emitByte(0x53);
    // push r12 (will hold locals ptr)
    emitByte(0x41); emitByte(0x54);
    // push r13 (will hold runtime ptr)
    emitByte(0x41); emitByte(0x55);
    // push r14 (scratch)
    emitByte(0x41); emitByte(0x56);
    // mov r14, rdi  (save original stack base)
    emitByte(0x49); emitByte(0x89); emitByte(0xFE);
    
    // mov rbx, rdi  (stack ptr = arg0)
    emitByte(0x48); emitByte(0x89); emitByte(0xFB);
    // mov r12, rsi  (locals ptr = arg1)
    emitByte(0x49); emitByte(0x89); emitByte(0xF4);
    // mov r13, rdx  (runtime ptr = arg2)
    emitByte(0x49); emitByte(0x89); emitByte(0xD5);
#elif JIT_ARCH_AARCH64
    // stp x29, x30, [sp, #-16]!
    emitBytes((const uint8_t*)"\xfd\x7b\xbf\xa9", 4);
    // mov x29, sp
    emitBytes((const uint8_t*)"\xfd\x03\x00\x91", 4);
    // Save arguments to callee-saved regs: x19=stack, x20=locals, x21=runtime
    // mov x19, x0
    emitBytes((const uint8_t*)"\xf3\x03\x00\xaa", 4);
    // mov x20, x1
    emitBytes((const uint8_t*)"\xf4\x03\x01\xaa", 4);
    // mov x21, x2
    emitBytes((const uint8_t*)"\xf5\x03\x02\xaa", 4);
#endif
}

void Compiler::emitEpilogue() {
#if JIT_ARCH_X86_64
    // pop r14
    emitByte(0x41); emitByte(0x5e);
    // pop r13
    emitByte(0x41); emitByte(0x5d);
    // pop r12
    emitByte(0x41); emitByte(0x5c);
    // pop rbx
    emitByte(0x5b);
    // pop rbp
    emitByte(0x5d);
    // ret
    emitByte(0xc3);
#elif JIT_ARCH_AARCH64
    // ldp x29, x30, [sp], #16
    emitBytes((const uint8_t*)"\xfd\x7b\xc1\xa8", 4);
    // ret
    emitBytes((const uint8_t*)"\xc0\x03\x5f\xd6", 4);
#endif
}

void Compiler::emitStencil(size_t stencilIndex) {
    const Stencil& stencil = STENCILS[stencilIndex];
    
    // Align emit offset for better cache performance
    while (emitOffset_ % kCodeAlignment != 0) {
        emitByte(0x90);  // NOP
    }
    
    // Copy stencil code
    size_t stencilStart = emitOffset_;
    emitBytes(stencil.code, stencil.codeSize);
    
    // Record that we emitted this stencil (for later hole patching)
    // Actual patching happens later with concrete values
}

void Compiler::patchHole(size_t codeOffset, size_t holeOffset, uint64_t value) {
    size_t patchOffset = codeOffset + holeOffset;
    if (patchOffset + 8 <= emitBuffer_.size()) {
        std::memcpy(&emitBuffer_[patchOffset], &value, 8);
    }
}

void Compiler::createLabel(size_t bytecodeIP) {
    if (labels_.find(bytecodeIP) == labels_.end()) {
        labels_[bytecodeIP] = Label{};
    }
}

void Compiler::emitJumpToLabel(size_t bytecodeIP) {
    // Record that this location needs to jump to the label
    createLabel(bytecodeIP);
    
    // Emit placeholder jump (will be patched)
#if JIT_ARCH_X86_64
    // jmp rel32 (placeholder)
    emitByte(0xe9);
    // Now record the patchSite - it should point to where the rel32 starts
    labels_[bytecodeIP].patchSites.push_back(emitOffset_);
    emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
#elif JIT_ARCH_AARCH64
    // For aarch64, the whole instruction is patched
    labels_[bytecodeIP].patchSites.push_back(emitOffset_);
    // b #0 (placeholder)
    emitBytes((const uint8_t*)"\x00\x00\x00\x14", 4);
#endif
}

void Compiler::resolveLabel(size_t bytecodeIP) {
    createLabel(bytecodeIP);
    labels_[bytecodeIP].targetOffset = emitOffset_;
#ifdef QZ_JIT_DEBUG
    std::cerr << "[JIT] Resolved label: bytecode IP " << bytecodeIP 
              << " -> code offset " << emitOffset_ << std::endl;
#endif
}

void Compiler::patchJumps() {
    for (auto& [ip, label] : labels_) {
        if (label.targetOffset == SIZE_MAX) {
#ifdef QZ_JIT_DEBUG
            std::cerr << "[JIT] Warning: unresolved label at bytecode IP " << ip << std::endl;
#endif
            continue;  // Unresolved
        }
        
        for (size_t patchSite : label.patchSites) {
#if JIT_ARCH_X86_64
            // Calculate relative offset for x86-64 jmp/jcc
            // patchSite points to the start of the 4-byte rel32
            // The instruction ends at patchSite + 4, so relative offset is from there
            int32_t rel = static_cast<int32_t>(label.targetOffset - (patchSite + 4));
#ifdef QZ_JIT_DEBUG
            std::cerr << "[JIT] Patching jump at code offset " << patchSite 
                      << " -> target " << label.targetOffset << " (rel=" << rel << ")" << std::endl;
#endif
            std::memcpy(&emitBuffer_[patchSite], &rel, 4);
#elif JIT_ARCH_AARCH64
            // Calculate relative offset for aarch64 b
            // b instruction is 4 bytes, offset in words
            int32_t wordOffset = static_cast<int32_t>((label.targetOffset - patchSite) / 4);
            uint32_t insn = 0x14000000 | (wordOffset & 0x3FFFFFF);
            std::memcpy(&emitBuffer_[patchSite], &insn, 4);
#endif
        }
    }
}

bool Compiler::canCompile(const bc::Function& fn) {
    // Check each opcode to see if we can handle it
    const auto& code = fn.code;
    size_t ip = 0;
    
    while (ip < code.size()) {
        auto opcode = static_cast<bc::OpCode>(code[ip++]);
        fprintf(stderr, "[JIT] canCompile ip=%zu opcode=%d\n", ip-1, static_cast<int>(opcode));
        
        switch (opcode) {
            // Supported opcodes - must match compileOpcode
            case bc::OpCode::NOP:
                break;
            case bc::OpCode::PUSH_INT32:
                ip += 4;  // Skip immediate
                break;
            case bc::OpCode::PUSH_DOUBLE64:
                ip += 8;
                break;
            case bc::OpCode::PUSH_BOOL:
                ip += 1;
                break;
            case bc::OpCode::PUSH_INT32_0:
            case bc::OpCode::PUSH_INT32_1:
            case bc::OpCode::PUSH_INT32_NEG1:
            case bc::OpCode::PUSH_TRUE:
            case bc::OpCode::PUSH_FALSE:
            case bc::OpCode::PUSH_NULL:
            case bc::OpCode::POP:
            case bc::OpCode::RETURN_VALUE:
            case bc::OpCode::RETURN_VOID:
                break;
            case bc::OpCode::LOAD_SLOT:
            case bc::OpCode::STORE_SLOT:
                ip += 2;  // u16 slot
                break;
            case bc::OpCode::LOAD_SLOT_0:
            case bc::OpCode::STORE_SLOT_0:
                break;
            case bc::OpCode::BINARY_OP:
                ip += 1;  // op
                break;
            case bc::OpCode::UNARY_OP:
                ip += 1;
                break;
            case bc::OpCode::JUMP:
            case bc::OpCode::JUMP_IF_FALSE:
            case bc::OpCode::JUMP_IF_TRUE:
                ip += 4;  // rel32
                break;
            case bc::OpCode::INCREMENT_SLOT:
            case bc::OpCode::DECREMENT_SLOT:
                ip += 2;
                break;
            case bc::OpCode::LOOP_COND_SLOT_LT_INT32:
                ip += 2 + 4 + 4;  // slot, limit, rel
                break;
            case bc::OpCode::LOAD_SLOT_PUSH_INT32:
                ip += 2 + 4;  // slot, int32
                break;
            case bc::OpCode::BINARY_OP_STORE_SLOT:
                ip += 1 + 2;  // op, slot
                break;

            // Function calls
            case bc::OpCode::CALL_NAME:
                ip += 4 + 1;  // u32 name, u8 argc
                break;
            case bc::OpCode::CALL_NAME_0:
            case bc::OpCode::CALL_NAME_1:
            case bc::OpCode::CALL_NAME_2:
                ip += 4;  // u32 name
                break;

            // Newly supported opcodes for IO and lambdas
            case bc::OpCode::PUSH_STRING:
                ip += 4;  // u32 stringIndex
                break;
            case bc::OpCode::MAKE_LAMBDA:
                ip += 4;  // u32 functionIndex
                break;
            case bc::OpCode::DECLARE_LAMBDA:
                ip += 4 + 4;  // u32 varName, u32 functionIndex
                break;
            case bc::OpCode::DEF_FUNCTION:
                ip += 4 + 4;  // u32 nameStringIndex, u32 functionIndex
                break;

            // Array access
            case bc::OpCode::INDEX_GET:
                ip += 4;  // u32 varName
                break;
                
            // Unsupported - fall back to interpreter
            // These require runtime support or complex operations
            case bc::OpCode::NEW_OBJECT:
            case bc::OpCode::LOAD_VAR:
            case bc::OpCode::STORE_VAR:
            case bc::OpCode::DECLARE_ARRAY:
            case bc::OpCode::DECLARE_DICT:
            case bc::OpCode::MAKE_ARRAY_EXPR:
            case bc::OpCode::MAKE_DICT_EXPR:
            case bc::OpCode::DEF_CLASS:
            case bc::OpCode::DEF_INTERFACE:
            case bc::OpCode::TRY_PUSH:
            case bc::OpCode::TRY_POP:
            case bc::OpCode::THROW_VALUE:
            case bc::OpCode::THROW_NEW:
            default:
                fprintf(stderr, "[JIT] canCompile failed at ip=%zu opcode=%d\n", ip-1, static_cast<int>(opcode));
#ifdef QZ_JIT_DEBUG
                std::cerr << "[JIT] canCompile failed at ip=" << (ip-1) 
                          << " opcode=" << static_cast<int>(opcode) << std::endl;
#endif
                return false;
        }
    }
    
    return true;
}

static bool hasLoop(const bc::Function& fn) {
    const auto& code = fn.code;
    size_t ip = 0;
    while (ip < code.size()) {
        auto opcode = static_cast<bc::OpCode>(code[ip++]);
        if (opcode == bc::OpCode::LOOP_COND_SLOT_LT_INT32) {
            return true;
        }
        // skip immediates based on opcode (similar to canCompile)
        switch (opcode) {
            case bc::OpCode::PUSH_INT32:
                ip += 4;
                break;
            case bc::OpCode::PUSH_DOUBLE64:
                ip += 8;
                break;
            case bc::OpCode::PUSH_BOOL:
                ip += 1;
                break;
            case bc::OpCode::PUSH_STRING:
                ip += 4;
                break;
            case bc::OpCode::LOAD_VAR:
            case bc::OpCode::STORE_VAR:
            case bc::OpCode::CALL_NAME:
            case bc::OpCode::CALL_NAME_0:
            case bc::OpCode::CALL_NAME_1:
            case bc::OpCode::CALL_NAME_2:
            case bc::OpCode::NEW_OBJECT:
            case bc::OpCode::DEF_FUNCTION:
            case bc::OpCode::INDEX_GET:
            case bc::OpCode::DEF_CLASS:
            case bc::OpCode::DEF_INTERFACE:
            case bc::OpCode::TRY_PUSH:
            case bc::OpCode::THROW_NEW:
            case bc::OpCode::SET_CURRENT_MODULE:
                ip += 4;
                break;
            case bc::OpCode::DECLARE_ARRAY:
            case bc::OpCode::DECLARE_DICT:
            case bc::OpCode::DECLARE_LAMBDA:
            case bc::OpCode::MAKE_LAMBDA:
            case bc::OpCode::MAKE_ARRAY_EXPR:
            case bc::OpCode::MAKE_DICT_EXPR:
                // variable size, skip minimal to avoid misreading
                // For simplicity, assume no loop inside these complex ops
                return false;
            case bc::OpCode::LOAD_SLOT:
            case bc::OpCode::STORE_SLOT:
            case bc::OpCode::INCREMENT_SLOT:
            case bc::OpCode::DECREMENT_SLOT:
            case bc::OpCode::LOAD_SLOT_PUSH_INT32:
            case bc::OpCode::BINARY_OP_STORE_SLOT:
                ip += 2;
                break;
            case bc::OpCode::LOOP_COND_SLOT_LT_INT32:
                // already handled
                break;
            case bc::OpCode::JUMP:
            case bc::OpCode::JUMP_IF_FALSE:
            case bc::OpCode::JUMP_IF_TRUE:
                ip += 4;
                break;
            case bc::OpCode::BINARY_OP:
            case bc::OpCode::UNARY_OP:
                ip += 1;
                break;
            default:
                // no immediates
                break;
        }
    }
    return false;
}

// Analyze slot usage and assign registers for up to 2 most used slots
void Compiler::analyzeSlotUsage(const bc::Function& fn) {
    slotToReg_.clear();
    regToSlot_.clear();
    usedSlots_.clear();
    
    // Count slot usage frequency
    std::unordered_map<uint16_t, int> slotCounts;
    const auto& code = fn.code;
    size_t ip = 0;
    while (ip < code.size()) {
        auto opcode = static_cast<bc::OpCode>(code[ip++]);
        // skip immediates based on opcode
        switch (opcode) {
            case bc::OpCode::PUSH_INT32:
                ip += 4;
                break;
            case bc::OpCode::PUSH_DOUBLE64:
                ip += 8;
                break;
            case bc::OpCode::PUSH_BOOL:
                ip += 1;
                break;
            case bc::OpCode::PUSH_STRING:
                ip += 4;
                break;
            case bc::OpCode::LOAD_VAR:
            case bc::OpCode::STORE_VAR:
            case bc::OpCode::CALL_NAME:
            case bc::OpCode::CALL_NAME_0:
            case bc::OpCode::CALL_NAME_1:
            case bc::OpCode::CALL_NAME_2:
            case bc::OpCode::NEW_OBJECT:
            case bc::OpCode::DEF_FUNCTION:
            case bc::OpCode::INDEX_GET:
            case bc::OpCode::DEF_CLASS:
            case bc::OpCode::DEF_INTERFACE:
            case bc::OpCode::TRY_PUSH:
            case bc::OpCode::THROW_NEW:
            case bc::OpCode::SET_CURRENT_MODULE:
                ip += 4;
                break;
            case bc::OpCode::DECLARE_ARRAY:
            case bc::OpCode::DECLARE_DICT:
            case bc::OpCode::DECLARE_LAMBDA:
            case bc::OpCode::MAKE_LAMBDA:
            case bc::OpCode::MAKE_ARRAY_EXPR:
            case bc::OpCode::MAKE_DICT_EXPR:
                // variable size, skip minimal to avoid misreading
                // For simplicity, assume no caching for these
                ip = code.size(); // break out of loop
                break;
            case bc::OpCode::LOAD_SLOT:
            case bc::OpCode::STORE_SLOT:
            case bc::OpCode::INCREMENT_SLOT:
            case bc::OpCode::DECREMENT_SLOT:
            case bc::OpCode::LOAD_SLOT_PUSH_INT32:
            case bc::OpCode::BINARY_OP_STORE_SLOT:
                {
                    uint16_t slot;
                    std::memcpy(&slot, &code[ip], 2);
                    slotCounts[slot]++;
                    ip += 2;
                }
                break;
            case bc::OpCode::LOAD_SLOT_0:
            case bc::OpCode::STORE_SLOT_0:
                slotCounts[0]++;
                break;
            case bc::OpCode::LOOP_COND_SLOT_LT_INT32:
                // slot is in immediate bytes 0-1
                {
                    uint16_t slot;
                    std::memcpy(&slot, &code[ip], 2);
                    slotCounts[slot]++;
                    ip += 2 + 4 + 4; // slot, limit, rel
                }
                break;
            case bc::OpCode::JUMP:
            case bc::OpCode::JUMP_IF_FALSE:
            case bc::OpCode::JUMP_IF_TRUE:
                ip += 4;
                break;
            case bc::OpCode::BINARY_OP:
            case bc::OpCode::UNARY_OP:
                ip += 1;
                break;
            default:
                // no immediates
                break;
        }
    }
    
    // Sort slots by frequency
    std::vector<std::pair<uint16_t, int>> sorted(slotCounts.begin(), slotCounts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
#ifdef QZ_JIT_DEBUG
    std::cerr << "[JIT] Slot usage analysis:" << std::endl;
    for (const auto& [slot, count] : sorted) {
        std::cerr << "  slot " << slot << ": " << count << " accesses" << std::endl;
    }
#endif
    
    // Assign registers to up to 4 most used slots
    constexpr uint8_t kMaxCachedSlots = 4;
    uint8_t nextReg = 1; // start from xmm1 (xmm0 used for temporaries)
    for (size_t i = 0; i < sorted.size() && i < kMaxCachedSlots; ++i) {
        uint16_t slot = sorted[i].first;
        slotToReg_[slot] = nextReg;
        regToSlot_[nextReg] = slot;
        usedSlots_.push_back(slot);
#ifdef QZ_JIT_DEBUG
        std::cerr << "[JIT]   slot " << slot << " -> xmm" << static_cast<int>(nextReg) << std::endl;
#endif
        ++nextReg;
    }
}

// =============================================================================
// Direct x86-64 Code Emission Helpers
// =============================================================================

#if JIT_ARCH_X86_64

// Helper to emit ModR/M byte
static inline uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (mod << 6) | (reg << 3) | rm;
}

// Emit: mov dword [rbx + offset], imm32
void Compiler::emitMovMemImm32(int8_t offset, int32_t imm) {
    emitByte(0xC7);  // mov r/m32, imm32
    if (offset == 0) {
        emitByte(modrm(0, 0, 3));  // [rbx]
    } else {
        emitByte(modrm(1, 0, 3));  // [rbx + disp8]
        emitByte(static_cast<uint8_t>(offset));
    }
    emitByte(imm & 0xFF);
    emitByte((imm >> 8) & 0xFF);
    emitByte((imm >> 16) & 0xFF);
    emitByte((imm >> 24) & 0xFF);
}

// Emit: mov byte [rbx + offset], imm8
void Compiler::emitMovMemImm8(int8_t offset, uint8_t imm) {
    emitByte(0xC6);  // mov r/m8, imm8
    emitByte(modrm(1, 0, 3));  // [rbx + disp8]
    emitByte(static_cast<uint8_t>(offset));
    emitByte(imm);
}

// Emit: add rbx, imm8
void Compiler::emitAddRbxImm8(int8_t imm) {
    emitByte(0x48);  // REX.W
    emitByte(0x83);  // add r/m64, imm8
    emitByte(modrm(3, 0, 3));  // rbx
    emitByte(static_cast<uint8_t>(imm));
}

// Emit: sub rbx, imm8
void Compiler::emitSubRbxImm8(int8_t imm) {
    emitByte(0x48);  // REX.W
    emitByte(0x83);  // sub r/m64, imm8
    emitByte(modrm(3, 5, 3));  // rbx, sub
    emitByte(static_cast<uint8_t>(imm));
}

// Emit: sub rbx, imm32
void Compiler::emitSubRbxImm32(int32_t imm) {
    emitByte(0x48);  // REX.W
    emitByte(0x81);  // sub r/m64, imm32
    emitByte(modrm(3, 5, 3));  // rbx, sub
    emitByte(imm & 0xFF);
    emitByte((imm >> 8) & 0xFF);
    emitByte((imm >> 16) & 0xFF);
    emitByte((imm >> 24) & 0xFF);
}

// Emit: mov rax, [r12 + slot*16]  (load from locals)
void Compiler::emitLoadLocal(uint16_t slot) {
    int32_t offset = static_cast<int32_t>(slot) * 16;
    // mov rax, [r12 + offset]
    emitByte(0x49);  // REX.WB (64-bit, r12 base)
    emitByte(0x8B);  // mov r64, r/m64
    if (offset == 0) {
        emitByte(modrm(0, 0, 4));  // rax, [r12]
        emitByte(0x24);  // SIB: base=r12
    } else if (offset <= 127) {
        emitByte(modrm(1, 0, 4));  // rax, [r12 + disp8]
        emitByte(0x24);
        emitByte(static_cast<uint8_t>(offset));
    } else {
        emitByte(modrm(2, 0, 4));  // rax, [r12 + disp32]
        emitByte(0x24);
        emitByte(offset & 0xFF);
        emitByte((offset >> 8) & 0xFF);
        emitByte((offset >> 16) & 0xFF);
        emitByte((offset >> 24) & 0xFF);
    }
}

// Emit: mov [r12 + slot*16], rax  (store to locals)
void Compiler::emitStoreLocal(uint16_t slot) {
    int32_t offset = static_cast<int32_t>(slot) * 16;
    // mov [r12 + offset], rax
    emitByte(0x49);  // REX.WB (64-bit, r12 base)
    emitByte(0x89);  // mov r/m64, r64
    if (offset == 0) {
        emitByte(modrm(0, 0, 4));  // [r12], rax
        emitByte(0x24);  // SIB: base=r12
    } else if (offset <= 127) {
        emitByte(modrm(1, 0, 4));  // [r12 + disp8], rax
        emitByte(0x24);
        emitByte(static_cast<uint8_t>(offset));
    } else {
        emitByte(modrm(2, 0, 4));  // [r12 + disp32], rax
        emitByte(0x24);
        emitByte(offset & 0xFF);
        emitByte((offset >> 8) & 0xFF);
        emitByte((offset >> 16) & 0xFF);
        emitByte((offset >> 24) & 0xFF);
    }
}

// Emit: movdqu xmmN, [r12 + slot*16]  (load slot to XMM register)
void Compiler::emitLoadSlotToXmm(uint8_t xmmReg, uint16_t slot) {
    int32_t offset = static_cast<int32_t>(slot) * 16;
    // F3 prefix for movdqu
    emitByte(0xF3);
    // REX prefix: 0x40 + REX.R (if xmmReg >= 8) + REX.B (for r12)
    uint8_t rex = 0x40;
    if (xmmReg >= 8) rex |= 0x04; // REX.R
    rex |= 0x01; // REX.B for r12
    emitByte(rex);
    emitByte(0x0F);
    emitByte(0x6F); // movdqu xmm, m128
    // modrm and sib
    uint8_t reg = xmmReg & 7;
    if (offset == 0) {
        emitByte(modrm(0, reg, 4)); // [r12]
        emitByte(0x24); // SIB: base=r12
    } else if (offset <= 127) {
        emitByte(modrm(1, reg, 4)); // [r12 + disp8]
        emitByte(0x24);
        emitByte(static_cast<uint8_t>(offset));
    } else {
        emitByte(modrm(2, reg, 4)); // [r12 + disp32]
        emitByte(0x24);
        emitByte(offset & 0xFF);
        emitByte((offset >> 8) & 0xFF);
        emitByte((offset >> 16) & 0xFF);
        emitByte((offset >> 24) & 0xFF);
    }
}

// Emit: movdqu [r12 + slot*16], xmmN  (store slot from XMM register)
void Compiler::emitStoreSlotFromXmm(uint8_t xmmReg, uint16_t slot) {
    int32_t offset = static_cast<int32_t>(slot) * 16;
    emitByte(0xF3);
    uint8_t rex = 0x40;
    if (xmmReg >= 8) rex |= 0x04; // REX.R
    rex |= 0x01; // REX.B for r12
    emitByte(rex);
    emitByte(0x0F);
    emitByte(0x7F); // movdqu m128, xmm
    uint8_t reg = xmmReg & 7;
    if (offset == 0) {
        emitByte(modrm(0, reg, 4));
        emitByte(0x24);
    } else if (offset <= 127) {
        emitByte(modrm(1, reg, 4));
        emitByte(0x24);
        emitByte(static_cast<uint8_t>(offset));
    } else {
        emitByte(modrm(2, reg, 4));
        emitByte(0x24);
        emitByte(offset & 0xFF);
        emitByte((offset >> 8) & 0xFF);
        emitByte((offset >> 16) & 0xFF);
        emitByte((offset >> 24) & 0xFF);
    }
}

// Emit loads for all hot slots assigned to registers
void Compiler::emitLoadHotSlots() {
    for (const auto& [slot, reg] : slotToReg_) {
        emitLoadSlotToXmm(reg, slot);
    }
}

// Flush dirty slots from registers back to memory
void Compiler::flushDirtySlots() {
    for (uint16_t slot : dirtySlots_) {
        auto it = slotToReg_.find(slot);
        if (it != slotToReg_.end()) {
            emitStoreSlotFromXmm(it->second, slot);
        }
    }
    dirtySlots_.clear();
}

// Emit: movdqu [rbx], xmmReg  (store XMM register to stack)
void Compiler::emitStoreXmmToStack(uint8_t xmmReg) {
    // F3 prefix
    emitByte(0xF3);
    // REX prefix if xmmReg >= 8
    if (xmmReg >= 8) {
        emitByte(0x44); // REX.R = 1, others 0
    }
    emitByte(0x0F);
    emitByte(0x11); // movdqu m128, xmm
    // modrm: mod=00, reg=xmmReg, r/m=011 (rbx)
    emitByte((xmmReg & 7) << 3 | 0x03);
}

// Emit: movdqu xmmReg, [rbx]  (load XMM register from stack)
void Compiler::emitLoadXmmFromStack(uint8_t xmmReg) {
    emitByte(0xF3);
    if (xmmReg >= 8) {
        emitByte(0x44); // REX.R = 1
    }
    emitByte(0x0F);
    emitByte(0x6F); // movdqu xmm, m128
    emitByte((xmmReg & 7) << 3 | 0x03);
}

#endif // JIT_ARCH_X86_64

bool Compiler::compileOpcode(const bc::Function& fn, const bc::Program& program,
                             size_t& ip, int& stackDelta) {
    const auto& code = fn.code;
    auto opcode = static_cast<bc::OpCode>(code[ip++]);
    
    // Mark this bytecode offset as a potential jump target
    resolveLabel(ip - 1);
    
#if JIT_ARCH_X86_64
    // Register convention:
    // RBX = operand stack pointer (points to next free slot)
    // R12 = locals pointer
    // R13 = runtime pointer
    // RAX, RCX, RDX = scratch
    
    switch (opcode) {
        case bc::OpCode::NOP:
            // Do nothing
            break;
            
        case bc::OpCode::PUSH_INT32: {
            int32_t imm;
            std::memcpy(&imm, &code[ip], 4);
            ip += 4;
            
            // mov dword [rbx], imm  (low 32 bits)
            emitByte(0xC7); emitByte(0x03);
            emitByte(imm & 0xFF);
            emitByte((imm >> 8) & 0xFF);
            emitByte((imm >> 16) & 0xFF);
            emitByte((imm >> 24) & 0xFF);
            // mov dword [rbx+4], sign_extend (high 32 bits)
            emitByte(0xC7); emitByte(0x43); emitByte(0x04);
            if (imm < 0) {
                emitByte(0xFF); emitByte(0xFF); emitByte(0xFF); emitByte(0xFF);
            } else {
                emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            }
            // mov byte [rbx+8], 0  (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_INT32_0: {
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 0  (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_INT32_1: {
            // mov qword [rbx], 1
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x01); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 0  (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_INT32_NEG1: {
            // mov qword [rbx], -1
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0xFF); emitByte(0xFF); emitByte(0xFF); emitByte(0xFF);
            // mov byte [rbx+8], 0  (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_TRUE: {
            // mov qword [rbx], 1
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x01); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 2  (TAG_BOOL)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x02);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_FALSE: {
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 2  (TAG_BOOL)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x02);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_NULL: {
            // Push empty/null value - represented as empty string (tag=3 for string)
            // For simplicity in JIT, we push 0 with a special tag
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 3  (TAG_STRING - empty string as null)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_STRING: {
            // Push string constant - for simplicity, push empty string
            // Skip string index (u32)
            ip += 4;
            // Same as PUSH_NULL - push empty string
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 3  (TAG_STRING)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_DOUBLE64: {
            uint64_t bits;
            std::memcpy(&bits, &code[ip], 8);
            ip += 8;
            
            // movabs rax, bits
            emitByte(0x48); emitByte(0xB8);
            for (int i = 0; i < 8; i++) emitByte((bits >> (i*8)) & 0xFF);
            // mov [rbx], rax
            emitByte(0x48); emitByte(0x89); emitByte(0x03);
            // mov byte [rbx+8], 1  (TAG_DOUBLE)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x01);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::PUSH_BOOL: {
            bool val = code[ip++] != 0;
            // mov qword [rbx], val
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(val ? 0x01 : 0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 2  (TAG_BOOL)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x02);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::POP: {
            // sub rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            stackDelta--;
            break;
        }
        
        case bc::OpCode::LOAD_SLOT:
        case bc::OpCode::LOAD_SLOT_0: {
            uint16_t slot = 0;
            if (opcode == bc::OpCode::LOAD_SLOT) {
                std::memcpy(&slot, &code[ip], 2);
                ip += 2;
            }
            
            auto it = slotToReg_.find(slot);
            if (it != slotToReg_.end()) {
                // Slot is in XMM register, store it to stack
                emitStoreXmmToStack(it->second);
            } else {
                int32_t offset = static_cast<int32_t>(slot) * 16;
                
                // movdqu xmm0, [r12 + offset]  (load 16-byte JITValue)
                emitByte(0xF3); emitByte(0x41); emitByte(0x0F); emitByte(0x6F);
                if (offset == 0) {
                    emitByte(0x04); emitByte(0x24);  // [r12]
                } else if (offset <= 127) {
                    emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
                } else {
                    emitByte(0x84); emitByte(0x24);
                    emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                    emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
                }
                // movdqu [rbx], xmm0
                emitByte(0xF3); emitByte(0x0F); emitByte(0x11); emitByte(0x03);
            }
            
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta++;
            break;
        }
        
        case bc::OpCode::STORE_SLOT:
        case bc::OpCode::STORE_SLOT_0: {
            uint16_t slot = 0;
            if (opcode == bc::OpCode::STORE_SLOT) {
                std::memcpy(&slot, &code[ip], 2);
                ip += 2;
            }
            
            // sub rbx, 16 (pop)
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            
            auto it = slotToReg_.find(slot);
            if (it != slotToReg_.end()) {
                // Slot is in XMM register, load value from stack into register and mark dirty
                uint8_t xmmReg = it->second;
                emitLoadXmmFromStack(xmmReg);
                dirtySlots_.insert(slot);
                // Do NOT store to memory yet (deferred until flush)
            } else {
                int32_t offset = static_cast<int32_t>(slot) * 16;
                // movdqu xmm0, [rbx]  (load 16-byte JITValue from stack)
                emitByte(0xF3); emitByte(0x0F); emitByte(0x6F); emitByte(0x03);
                // movdqu [r12 + offset], xmm0  (store to local)
                emitByte(0xF3); emitByte(0x41); emitByte(0x0F); emitByte(0x11);
                if (offset == 0) {
                    emitByte(0x04); emitByte(0x24);  // [r12]
                } else if (offset <= 127) {
                    emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
                } else {
                    emitByte(0x84); emitByte(0x24);
                    emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                    emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
                }
            }
            
            stackDelta--;
            break;
        }
        
        case bc::OpCode::BINARY_OP: {
            auto op = static_cast<bc::BinaryOp>(code[ip++]);
            
            // sub rbx, 16 (prepare for result in stack[-2])
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            
            // Check if left operand is a double (tag at [rbx-16+8])
            // mov al, [rbx-16+8]  (left tag)
            emitByte(0x8A); emitByte(0x43); emitByte(0xF8);
            // cmp al, 1  (TAG_DOUBLE)
            emitByte(0x3C); emitByte(0x01);
            // jne int_path
            emitByte(0x75);
            size_t intPathPatch = emitOffset_;
            emitByte(0x00);  // placeholder
            
            // === DOUBLE PATH ===
            // movsd xmm0, [rbx-16]  (left as double)
            emitByte(0xF2); emitByte(0x0F); emitByte(0x10); emitByte(0x43); emitByte(0xF0);
            // movsd xmm1, [rbx]     (right as double)
            emitByte(0xF2); emitByte(0x0F); emitByte(0x10); emitByte(0x0B);
            
            bool isComparison = false;
            switch (op) {
                case bc::BinaryOp::ADD:
                    // addsd xmm0, xmm1
                    emitByte(0xF2); emitByte(0x0F); emitByte(0x58); emitByte(0xC1);
                    break;
                case bc::BinaryOp::SUB:
                    // subsd xmm0, xmm1
                    emitByte(0xF2); emitByte(0x0F); emitByte(0x5C); emitByte(0xC1);
                    break;
                case bc::BinaryOp::MUL:
                    // mulsd xmm0, xmm1
                    emitByte(0xF2); emitByte(0x0F); emitByte(0x59); emitByte(0xC1);
                    break;
                case bc::BinaryOp::DIV:
                    // divsd xmm0, xmm1
                    emitByte(0xF2); emitByte(0x0F); emitByte(0x5E); emitByte(0xC1);
                    break;
                case bc::BinaryOp::EQ:
                case bc::BinaryOp::NE:
                case bc::BinaryOp::LT:
                case bc::BinaryOp::GT:
                case bc::BinaryOp::LE:
                case bc::BinaryOp::GE:
                    isComparison = true;
                    // ucomisd xmm0, xmm1
                    emitByte(0x66); emitByte(0x0F); emitByte(0x2E); emitByte(0xC1);
                    // setCC al  (use unsigned comparisons for floating point)
                    emitByte(0x0F);
                    switch (op) {
                        case bc::BinaryOp::EQ: emitByte(0x94); break;  // sete
                        case bc::BinaryOp::NE: emitByte(0x95); break;  // setne
                        case bc::BinaryOp::LT: emitByte(0x92); break;  // setb (below)
                        case bc::BinaryOp::GT: emitByte(0x97); break;  // seta (above)
                        case bc::BinaryOp::LE: emitByte(0x96); break;  // setbe
                        case bc::BinaryOp::GE: emitByte(0x93); break;  // setae
                        default: break;
                    }
                    emitByte(0xC0);  // al
                    // movzx rax, al
                    emitByte(0x48); emitByte(0x0F); emitByte(0xB6); emitByte(0xC0);
                    // mov [rbx-16], rax
                    emitByte(0x48); emitByte(0x89); emitByte(0x43); emitByte(0xF0);
                    // mov byte [rbx-16+8], 2  (TAG_BOOL)
                    emitByte(0xC6); emitByte(0x43); emitByte(0xF8); emitByte(0x02);
                    break;
                default:
                    return false;
            }
            
            if (!isComparison) {
                // movsd [rbx-16], xmm0  (store double result)
                emitByte(0xF2); emitByte(0x0F); emitByte(0x11); emitByte(0x43); emitByte(0xF0);
                // mov byte [rbx-16+8], 1  (TAG_DOUBLE)
                emitByte(0xC6); emitByte(0x43); emitByte(0xF8); emitByte(0x01);
            }
            
            // jmp done
            emitByte(0xEB);
            size_t donePatch = emitOffset_;
            emitByte(0x00);  // placeholder
            
            // === INTEGER PATH ===
            size_t intPathStart = emitOffset_;
            // Patch the jne to jump here
            emitBuffer_[intPathPatch] = static_cast<uint8_t>(intPathStart - (intPathPatch + 1));
            
            // mov rcx, [rbx]  (right operand)
            emitByte(0x48); emitByte(0x8B); emitByte(0x0B);
            // mov rax, [rbx-16]  (left operand)
            emitByte(0x48); emitByte(0x8B); emitByte(0x43); emitByte(0xF0);
            
            switch (op) {
                case bc::BinaryOp::ADD:
                    // add rax, rcx
                    emitByte(0x48); emitByte(0x01); emitByte(0xC8);
                    break;
                case bc::BinaryOp::SUB:
                    // sub rax, rcx
                    emitByte(0x48); emitByte(0x29); emitByte(0xC8);
                    break;
                case bc::BinaryOp::MUL:
                    // imul rax, rcx
                    emitByte(0x48); emitByte(0x0F); emitByte(0xAF); emitByte(0xC1);
                    break;
                case bc::BinaryOp::DIV:
                    // cqo; idiv rcx
                    emitByte(0x48); emitByte(0x99);  // cqo
                    emitByte(0x48); emitByte(0xF7); emitByte(0xF9);  // idiv rcx
                    break;
                case bc::BinaryOp::EQ:
                case bc::BinaryOp::NE:
                case bc::BinaryOp::LT:
                case bc::BinaryOp::GT:
                case bc::BinaryOp::LE:
                case bc::BinaryOp::GE:
                    // cmp rax, rcx
                    emitByte(0x48); emitByte(0x39); emitByte(0xC8);
                    // setCC al
                    emitByte(0x0F);
                    switch (op) {
                        case bc::BinaryOp::EQ: emitByte(0x94); break;  // sete
                        case bc::BinaryOp::NE: emitByte(0x95); break;  // setne
                        case bc::BinaryOp::LT: emitByte(0x9C); break;  // setl
                        case bc::BinaryOp::GT: emitByte(0x9F); break;  // setg
                        case bc::BinaryOp::LE: emitByte(0x9E); break;  // setle
                        case bc::BinaryOp::GE: emitByte(0x9D); break;  // setge
                        default: break;
                    }
                    emitByte(0xC0);  // al
                    // movzx rax, al
                    emitByte(0x48); emitByte(0x0F); emitByte(0xB6); emitByte(0xC0);
                    break;
                default:
                    return false;
            }
            
            // mov [rbx-16], rax  (store result)
            emitByte(0x48); emitByte(0x89); emitByte(0x43); emitByte(0xF0);
            
            if (isComparison) {
                // mov byte [rbx-16+8], 2  (TAG_BOOL)
                emitByte(0xC6); emitByte(0x43); emitByte(0xF8); emitByte(0x02);
            }
            // else tag stays as TAG_INT (0) from original value
            
            // === DONE ===
            size_t doneStart = emitOffset_;
            // Patch the jmp done
            emitBuffer_[donePatch] = static_cast<uint8_t>(doneStart - (donePatch + 1));
            
            stackDelta--;
            break;
        }
        
        case bc::OpCode::UNARY_OP: {
            auto op = static_cast<bc::UnaryOp>(code[ip++]);
            
            switch (op) {
                case bc::UnaryOp::NEG: {
                    // Check if operand is a double
                    // mov al, [rbx-16+8]  (tag)
                    emitByte(0x8A); emitByte(0x43); emitByte(0xF8);
                    // cmp al, 1  (TAG_DOUBLE)
                    emitByte(0x3C); emitByte(0x01);
                    // jne int_neg
                    emitByte(0x75);
                    size_t intNegPatch = emitOffset_;
                    emitByte(0x00);
                    
                    // Double negation: xor the sign bit
                    // mov rax, [rbx-16]
                    emitByte(0x48); emitByte(0x8B); emitByte(0x43); emitByte(0xF0);
                    // movabs rcx, 0x8000000000000000  (sign bit)
                    emitByte(0x48); emitByte(0xB9);
                    emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
                    emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x80);
                    // xor rax, rcx
                    emitByte(0x48); emitByte(0x31); emitByte(0xC8);
                    // mov [rbx-16], rax
                    emitByte(0x48); emitByte(0x89); emitByte(0x43); emitByte(0xF0);
                    // jmp done
                    emitByte(0xEB);
                    size_t donePatch = emitOffset_;
                    emitByte(0x00);
                    
                    // Integer negation
                    size_t intNegStart = emitOffset_;
                    emitBuffer_[intNegPatch] = static_cast<uint8_t>(intNegStart - (intNegPatch + 1));
                    // mov rax, [rbx-16]
                    emitByte(0x48); emitByte(0x8B); emitByte(0x43); emitByte(0xF0);
                    // neg rax
                    emitByte(0x48); emitByte(0xF7); emitByte(0xD8);
                    // mov [rbx-16], rax
                    emitByte(0x48); emitByte(0x89); emitByte(0x43); emitByte(0xF0);
                    
                    // Done
                    size_t doneStart = emitOffset_;
                    emitBuffer_[donePatch] = static_cast<uint8_t>(doneStart - (donePatch + 1));
                    break;
                }
                case bc::UnaryOp::NOT:
                    // mov rax, [rbx-16]
                    emitByte(0x48); emitByte(0x8B); emitByte(0x43); emitByte(0xF0);
                    // test rax, rax; sete al; movzx rax, al
                    emitByte(0x48); emitByte(0x85); emitByte(0xC0);
                    emitByte(0x0F); emitByte(0x94); emitByte(0xC0);
                    emitByte(0x48); emitByte(0x0F); emitByte(0xB6); emitByte(0xC0);
                    // mov [rbx-16], rax
                    emitByte(0x48); emitByte(0x89); emitByte(0x43); emitByte(0xF0);
                    // mov byte [rbx-16+8], 2  (TAG_BOOL)
                    emitByte(0xC6); emitByte(0x43); emitByte(0xF8); emitByte(0x02);
                    break;
                default:
                    return false;
            }
            break;
        }
        
        case bc::OpCode::INCREMENT_SLOT: {
            uint16_t slot;
            std::memcpy(&slot, &code[ip], 2);
            ip += 2;
            
            int32_t offset = static_cast<int32_t>(slot) * 16;
            // inc qword [r12 + offset]
            emitByte(0x49); emitByte(0xFF);
            if (offset == 0) {
                emitByte(0x04); emitByte(0x24);
            } else if (offset <= 127) {
                emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
            } else {
                emitByte(0x84); emitByte(0x24);
                emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
            }
            break;
        }
        
        case bc::OpCode::DECREMENT_SLOT: {
            uint16_t slot;
            std::memcpy(&slot, &code[ip], 2);
            ip += 2;
            
            int32_t offset = static_cast<int32_t>(slot) * 16;
            // dec qword [r12 + offset]
            emitByte(0x49); emitByte(0xFF);
            if (offset == 0) {
                emitByte(0x0C); emitByte(0x24);
            } else if (offset <= 127) {
                emitByte(0x4C); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
            } else {
                emitByte(0x8C); emitByte(0x24);
                emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
            }
            break;
        }
        
        case bc::OpCode::JUMP: {
            flushDirtySlots();
            int32_t rel;
            std::memcpy(&rel, &code[ip], 4);
            ip += 4;
            
            size_t targetIP = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
            emitJumpToLabel(targetIP);
            break;
        }
        
        case bc::OpCode::JUMP_IF_FALSE: {
            flushDirtySlots();
            int32_t rel;
            std::memcpy(&rel, &code[ip], 4);
            ip += 4;
            
            size_t targetIP = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
            
            // sub rbx, 16 (pop condition)
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            // mov rax, [rbx]
            emitByte(0x48); emitByte(0x8B); emitByte(0x03);
            // test rax, rax
            emitByte(0x48); emitByte(0x85); emitByte(0xC0);
            // je rel32
            emitByte(0x0F); emitByte(0x84);
            
            createLabel(targetIP);
            labels_[targetIP].patchSites.push_back(emitOffset_);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);  // placeholder
            
            stackDelta--;
            break;
        }
        
        case bc::OpCode::JUMP_IF_TRUE: {
            flushDirtySlots();
            int32_t rel;
            std::memcpy(&rel, &code[ip], 4);
            ip += 4;
            
            size_t targetIP = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
            
            // sub rbx, 16 (pop condition)
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            // mov rax, [rbx]
            emitByte(0x48); emitByte(0x8B); emitByte(0x03);
            // test rax, rax
            emitByte(0x48); emitByte(0x85); emitByte(0xC0);
            // jne rel32
            emitByte(0x0F); emitByte(0x85);
            
            createLabel(targetIP);
            labels_[targetIP].patchSites.push_back(emitOffset_);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            
            stackDelta--;
            break;
        }
        
        case bc::OpCode::LOOP_COND_SLOT_LT_INT32: {
            flushDirtySlots();
            // Optimized loop condition: if slot < limit, jump to target
            uint16_t slot;
            int32_t limit;
            int32_t rel;
            std::memcpy(&slot, &code[ip], 2); ip += 2;
            std::memcpy(&limit, &code[ip], 4); ip += 4;
            std::memcpy(&rel, &code[ip], 4); ip += 4;
            
            size_t targetIP = static_cast<size_t>(static_cast<int64_t>(ip) + rel);
            
            int32_t offset = static_cast<int32_t>(slot) * 16;
            
            // cmp qword [r12 + offset], limit
            emitByte(0x49); emitByte(0x81);  // REX.WB cmp r/m64, imm32
            if (offset == 0) {
                emitByte(0x3C); emitByte(0x24);  // /7 [r12]
            } else if (offset <= 127) {
                emitByte(0x7C); emitByte(0x24);  // /7 [r12 + disp8]
                emitByte(static_cast<uint8_t>(offset));
            } else {
                emitByte(0xBC); emitByte(0x24);  // /7 [r12 + disp32]
                emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
            }
            // emit limit as imm32
            emitByte(limit & 0xFF);
            emitByte((limit >> 8) & 0xFF);
            emitByte((limit >> 16) & 0xFF);
            emitByte((limit >> 24) & 0xFF);
            
            // jl rel32 (jump if less)
            emitByte(0x0F); emitByte(0x8C);
            createLabel(targetIP);
            labels_[targetIP].patchSites.push_back(emitOffset_);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            break;
        }
        
        case bc::OpCode::LOAD_SLOT_PUSH_INT32: {
            // Fused: LOAD_SLOT + PUSH_INT32
            uint16_t slot;
            int32_t imm;
            std::memcpy(&slot, &code[ip], 2); ip += 2;
            std::memcpy(&imm, &code[ip], 4); ip += 4;
            
            int32_t offset = static_cast<int32_t>(slot) * 16;
            
            // Load slot to stack
            // mov rax, [r12 + offset]
            emitByte(0x49); emitByte(0x8B);
            if (offset == 0) {
                emitByte(0x04); emitByte(0x24);
            } else if (offset <= 127) {
                emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
            } else {
                emitByte(0x84); emitByte(0x24);
                emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
            }
            // mov [rbx], rax
            emitByte(0x48); emitByte(0x89); emitByte(0x03);
            // mov al, [r12 + offset + 8]
            emitByte(0x41); emitByte(0x8A);
            if (offset + 8 <= 127) {
                emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset + 8));
            } else {
                emitByte(0x84); emitByte(0x24);
                int32_t tagOff = offset + 8;
                emitByte(tagOff & 0xFF); emitByte((tagOff >> 8) & 0xFF);
                emitByte((tagOff >> 16) & 0xFF); emitByte((tagOff >> 24) & 0xFF);
            }
            // mov [rbx+8], al
            emitByte(0x88); emitByte(0x43); emitByte(0x08);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            
            // Push int32
            // mov dword [rbx], imm
            emitByte(0xC7); emitByte(0x03);
            emitByte(imm & 0xFF); emitByte((imm >> 8) & 0xFF);
            emitByte((imm >> 16) & 0xFF); emitByte((imm >> 24) & 0xFF);
            // mov dword [rbx+4], sign extend
            emitByte(0xC7); emitByte(0x43); emitByte(0x04);
            if (imm < 0) {
                emitByte(0xFF); emitByte(0xFF); emitByte(0xFF); emitByte(0xFF);
            } else {
                emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            }
            // mov byte [rbx+8], 0  (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            
            stackDelta += 2;
            break;
        }
        
        case bc::OpCode::BINARY_OP_STORE_SLOT: {
            // Fused: BINARY_OP + STORE_SLOT
            // Stack: [... lhs, rhs] -> [...]  and stores result to slot
            auto op = static_cast<bc::BinaryOp>(code[ip++]);
            uint16_t slot;
            std::memcpy(&slot, &code[ip], 2); ip += 2;
            
            // Pop rhs: mov rcx, [rbx-16]
            emitByte(0x48); emitByte(0x8B); emitByte(0x4B); emitByte(0xF0);
            // Pop lhs: mov rax, [rbx-32]
            emitByte(0x48); emitByte(0x8B); emitByte(0x43); emitByte(0xE0);
            // sub rbx, 32 (pop both)
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x20);
            
            // Perform operation
            switch (op) {
                case bc::BinaryOp::ADD:
                    emitByte(0x48); emitByte(0x01); emitByte(0xC8);  // add rax, rcx
                    break;
                case bc::BinaryOp::SUB:
                    emitByte(0x48); emitByte(0x29); emitByte(0xC8);  // sub rax, rcx
                    break;
                case bc::BinaryOp::MUL:
                    emitByte(0x48); emitByte(0x0F); emitByte(0xAF); emitByte(0xC1);  // imul rax, rcx
                    break;
                case bc::BinaryOp::DIV:
                    emitByte(0x48); emitByte(0x99);  // cqo
                    emitByte(0x48); emitByte(0xF7); emitByte(0xF9);  // idiv rcx
                    break;
                default:
                    // For comparisons, do cmp + setcc
                    emitByte(0x48); emitByte(0x39); emitByte(0xC8);  // cmp rax, rcx
                    emitByte(0x0F);
                    switch (op) {
                        case bc::BinaryOp::EQ: emitByte(0x94); break;
                        case bc::BinaryOp::NE: emitByte(0x95); break;
                        case bc::BinaryOp::LT: emitByte(0x9C); break;
                        case bc::BinaryOp::GT: emitByte(0x9F); break;
                        case bc::BinaryOp::LE: emitByte(0x9E); break;
                        case bc::BinaryOp::GE: emitByte(0x9D); break;
                        default: return false;
                    }
                    emitByte(0xC0);  // al
                    emitByte(0x48); emitByte(0x0F); emitByte(0xB6); emitByte(0xC0);  // movzx rax, al
                    break;
            }
            
            // Check if slot is cached in XMM register
            auto it = slotToReg_.find(slot);
            if (it != slotToReg_.end()) {
                // Slot is cached in XMM register
                uint8_t xmmReg = it->second;
                
                // Store result (RAX) to temporary memory [r14] and tag 0
                // mov [r14], rax
                emitByte(0x49); emitByte(0x89); emitByte(0x04); emitByte(0x26);
                // mov byte [r14+8], 0  (TAG_INT)
                emitByte(0x41); emitByte(0xC6); emitByte(0x44); emitByte(0x26); emitByte(0x08); emitByte(0x00);
                
                // Load into XMM register
                // movdqu xmmN, [r14]
                uint8_t reg = xmmReg & 7;
                uint8_t rex = 0x40;
                if (xmmReg >= 8) rex |= 0x04; // REX.R
                rex |= 0x01; // REX.B for r14
                emitByte(rex);
                emitByte(0x0F);
                emitByte(0x6F); // movdqu xmm, m128
                emitByte(modrm(0, reg, 4)); // [r14] with SIB
                emitByte(0x26); // SIB: base=r14, index=none
                
                // Mark slot dirty (register value differs from memory)
                dirtySlots_.insert(slot);
            } else {
                // Slot not cached, store to memory as usual
                int32_t offset = static_cast<int32_t>(slot) * 16;
                // mov [r12 + offset], rax
                emitByte(0x49); emitByte(0x89);
                if (offset == 0) {
                    emitByte(0x04); emitByte(0x24);
                } else if (offset <= 127) {
                    emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset));
                } else {
                    emitByte(0x84); emitByte(0x24);
                    emitByte(offset & 0xFF); emitByte((offset >> 8) & 0xFF);
                    emitByte((offset >> 16) & 0xFF); emitByte((offset >> 24) & 0xFF);
                }
                // Store tag (TAG_INT = 0)
                // mov byte [r12 + offset + 8], 0
                emitByte(0x41); emitByte(0xC6);
                if (offset + 8 <= 127) {
                    emitByte(0x44); emitByte(0x24); emitByte(static_cast<uint8_t>(offset + 8));
                } else {
                    emitByte(0x84); emitByte(0x24);
                    int32_t tagOff = offset + 8;
                    emitByte(tagOff & 0xFF); emitByte((tagOff >> 8) & 0xFF);
                    emitByte((tagOff >> 16) & 0xFF); emitByte((tagOff >> 24) & 0xFF);
                }
                emitByte(0x00);  // TAG_INT
            }
            
            stackDelta -= 2;
            break;
        }
        
        case bc::OpCode::CALL_NAME: {
            uint32_t nameIdx;
            uint8_t argc;
            std::memcpy(&nameIdx, &code[ip], 4);
            std::memcpy(&argc, &code[ip + 4], 1);
            ip += 5;
            // Pop argc values
            if (argc > 0) {
                int32_t offset = static_cast<int32_t>(argc) * 16;
                if (offset <= 127) {
                    emitSubRbxImm8(static_cast<int8_t>(offset));
                } else {
                    emitSubRbxImm32(offset);
                }
                stackDelta -= argc;
            }
            // Push null (empty string)
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 3  (TAG_STRING)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta += 1;
            break;
        }
        case bc::OpCode::CALL_NAME_0: {
            uint32_t nameIdx;
            std::memcpy(&nameIdx, &code[ip], 4);
            ip += 4;
            // No arguments to pop
            // Push null
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta += 1;
            break;
        }
        case bc::OpCode::CALL_NAME_1: {
            uint32_t nameIdx;
            std::memcpy(&nameIdx, &code[ip], 4);
            ip += 4;
            // Pop 1 argument
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            // Push null
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta += 0; // -1 + 1 = 0
            break;
        }
        case bc::OpCode::CALL_NAME_2: {
            uint32_t nameIdx;
            std::memcpy(&nameIdx, &code[ip], 4);
            ip += 4;
            // Pop 2 arguments
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x20);
            // Push null
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x03);
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta -= 1; // -2 + 1 = -1
            break;
        }
        
        case bc::OpCode::LOAD_VAR: {
            uint32_t nameIdx;
            std::memcpy(&nameIdx, &code[ip], 4);
            ip += 4;
            // Push zero integer
            // mov qword [rbx], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x03);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx+8], 0 (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0x08); emitByte(0x00);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            stackDelta += 1;
            break;
        }
        
        case bc::OpCode::STORE_VAR: {
            uint32_t nameIdx;
            std::memcpy(&nameIdx, &code[ip], 4);
            ip += 4;
            // Pop one value (size 16)
            emitByte(0x48); emitByte(0x83); emitByte(0xEB); emitByte(0x10);
            stackDelta -= 1;
            break;
        }
        
        case bc::OpCode::INDEX_GET: {
            uint32_t varName;
            std::memcpy(&varName, &code[ip], 4);
            ip += 4;
            // Replace top of stack (index) with zero integer element
            // mov qword [rbx - 16], 0
            emitByte(0x48); emitByte(0xC7); emitByte(0x43); emitByte(0xF0);
            emitByte(0x00); emitByte(0x00); emitByte(0x00); emitByte(0x00);
            // mov byte [rbx - 8], 0 (TAG_INT)
            emitByte(0xC6); emitByte(0x43); emitByte(0xF8); emitByte(0x00);
            break;
        }
        
        case bc::OpCode::RETURN_VALUE:
            // Move top value (at [rbx - 16]) to original stack base (r14)
            // movdqu xmm0, [rbx - 16]
            emitByte(0xF3); emitByte(0x0F); emitByte(0x6F); emitByte(0x43); emitByte(0xF0);
            // movups [r14], xmm0
            emitByte(0xF3); emitByte(0x41); emitByte(0x0F); emitByte(0x11); emitByte(0x06);
            // Set stack pointer to point after result (base + 16)
            // mov rbx, r14
            emitByte(0x4C); emitByte(0x89); emitByte(0xF3);
            // add rbx, 16
            emitByte(0x48); emitByte(0x83); emitByte(0xC3); emitByte(0x10);
            emitEpilogue();
            break;
        
        case bc::OpCode::RETURN_VOID:
            // Just emit epilogue - result is already on stack
            emitEpilogue();
            break;

        case bc::OpCode::DEF_FUNCTION:
            // Skip function definition (already registered by interpreter)
            ip += 8; // u32 nameStringIndex, u32 functionIndex
            break;

        case bc::OpCode::SET_CURRENT_MODULE:
            // Skip module path string index (u32)
            ip += 4;
            break;

        case bc::OpCode::CLEAR_CURRENT_MODULE:
            // No operand
            break;

        default:
            // Unsupported opcode
            return false;
    }
    
    return true;
    
#else
    // Non-x86_64 architecture - not supported yet
    (void)fn;
    (void)program;
    (void)ip;
    (void)stackDelta;
    return false;
#endif
}

CompiledFunction* Compiler::compile(const bc::Program& program, uint32_t functionIndex) {
    if (functionIndex >= program.functions.size()) {
        return nullptr;
    }
    
    const bc::Function& fn = program.functions[functionIndex];
    
    // Check if compilable
    if (!canCompile(fn)) {
        return nullptr;
    }

    // Analyze slot usage and allocate registers for hot slots
    analyzeSlotUsage(fn);
#ifdef QZ_JIT_DEBUG
    std::cerr << "[JIT] compile: analyzed slot usage for function " << functionIndex << std::endl;
#endif

    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Reset emission state
    resetEmitState();
    
    // Emit prologue
    emitPrologue();

    // Load hot slots into XMM registers
    emitLoadHotSlots();
    
    // Compile bytecode
    size_t ip = 0;
    int stackDelta = 0;
    
    while (ip < fn.code.size()) {
        if (!compileOpcode(fn, program, ip, stackDelta)) {
            // Compilation failed - opcode not supported
            return nullptr;
        }
    }
    
    // Flush dirty slots back to memory
    flushDirtySlots();
    // Emit epilogue
    emitEpilogue();
    
    // Patch all jumps
    patchJumps();
    
    // Allocate executable memory
    auto compiled = std::make_unique<CompiledFunction>();
    if (!compiled->code.allocate(emitOffset_)) {
        return nullptr;
    }
    
    // Copy code
    std::memcpy(compiled->code.data(), emitBuffer_.data(), emitOffset_);
    compiled->codeSize = emitOffset_;
    
    // Make executable
    if (!compiled->code.makeExecutable()) {
        return nullptr;
    }
    
    compiled->functionIndex = functionIndex;
    compiled->isValid = true;
    
    // Update statistics
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
    
    stats_.functionsCompiled++;
    stats_.bytecodesCompiled += fn.code.size();
    stats_.nativeCodeBytes += emitOffset_;
    stats_.compilationTimeNs += duration.count();
    
    return compiled.release();
}

// =============================================================================
// Engine Implementation
// =============================================================================

Engine::Engine(Runtime& runtime)
    : runtime_(runtime)
    , compiler_(std::make_unique<Compiler>(runtime)) {
}

Engine::~Engine() = default;

void Engine::recordCall(uint32_t functionIndex) {
    callCounts_[functionIndex]++;
    stats_.totalCalls++;
}

bool Engine::shouldCompile(uint32_t functionIndex) const {
    if (!enabled_) return false;
    
    auto it = callCounts_.find(functionIndex);
    if (it == callCounts_.end()) return false;
    
    return it->second >= threshold_;
}

CompiledFunction* Engine::getCompiled(const bc::Program& program, uint32_t functionIndex) {
    // Check cache
    auto it = compiledCache_.find(functionIndex);
    if (it != compiledCache_.end()) {
        return it->second.get();
    }
    
    // Check if function is compilable FIRST (before threshold check)
    // This ensures all compilable functions get JIT compiled immediately for language-wide JIT support
    bool isCompilable = false;
    if (functionIndex < program.functions.size()) {
        const bc::Function& fn = program.functions[functionIndex];
        isCompilable = compiler_->canCompile(fn);
        
        if (isCompilable) {
            // Force compilation for all compilable functions - language-wide JIT support
#ifdef QZ_JIT_DEBUG
            std::cerr << "[JIT] Function #" << functionIndex 
                      << " is compilable (" << fn.code.size() << " bytes), forcing immediate compilation" << std::endl;
#endif
            callCounts_[functionIndex] = threshold_;
        }
    }
    
    // Should we compile?
    bool needCompile = shouldCompile(functionIndex);
    if (!needCompile) {
        return nullptr;
    }
    
    // Try to compile
#ifdef QZ_JIT_DEBUG
    std::cerr << "[JIT] Compiling function #" << functionIndex;
    if (functionIndex < program.functions.size()) {
        std::cerr << " (" << program.functions[functionIndex].code.size() << " bytes of bytecode)";
    }
    std::cerr << std::endl;
#endif
    
    CompiledFunction* compiled = compiler_->compile(program, functionIndex);
    
    if (compiled) {
        stats_.compilations++;
        compiledCache_[functionIndex].reset(compiled);
#ifdef QZ_JIT_DEBUG
        std::cerr << "[JIT] Successfully compiled function #" << functionIndex 
                  << " -> " << compiled->codeSize << " bytes native code" << std::endl;
#endif
    } else {
        stats_.compilationFailures++;
        // Cache the failure to avoid repeated attempts
        compiledCache_[functionIndex] = nullptr;
#ifdef QZ_JIT_DEBUG
        std::cerr << "[JIT] Failed to compile function #" << functionIndex 
                  << " (unsupported opcodes)" << std::endl;
#endif
    }
    
    return compiled;
}

bool Engine::tryExecute(const bc::Program& program, uint32_t functionIndex,
                       JITValue* stack, JITValue* locals) {
    CompiledFunction* compiled = getCompiled(program, functionIndex);
    
    if (!compiled || !compiled->isValid) {
        return false;
    }
    
    // Execute the compiled code
    JITFunction fn = compiled->getEntryPoint();
    fn(stack, locals, &runtime_);
    
    stats_.jitExecutions++;
    return true;
}

void Engine::clear() {
    compiledCache_.clear();
    callCounts_.clear();
}

// =============================================================================
// Value Conversion Utilities
// =============================================================================

JITValue valueToJIT(const Value& v) {
    if (auto* i = std::get_if<int>(&v)) {
        return JITValue::makeInt(*i);
    } else if (auto* d = std::get_if<double>(&v)) {
        return JITValue::makeDouble(*d);
    } else if (auto* b = std::get_if<bool>(&v)) {
        return JITValue::makeBool(*b);
    }
    // Default to int 0 for unsupported types
    return JITValue::makeInt(0);
}

Value jitToValue(const JITValue& v) {
    switch (v.tag) {
        case JITValue::TAG_INT:
            return Value(static_cast<int>(v.asInt()));
        case JITValue::TAG_DOUBLE:
            return Value(v.asDouble());
        case JITValue::TAG_BOOL:
            return Value(v.asBool());
        default:
            return Value(0);
    }
}

} // namespace qz::jit
