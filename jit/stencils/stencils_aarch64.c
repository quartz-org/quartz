/**
 * =============================================================================
 * Quartz JIT Stencils - ARM64/AArch64 (Apple Silicon, Linux ARM64)
 * =============================================================================
 * 
 * Copy-and-Patch code templates for ARM64 architecture.
 * Compatible with: Apple Silicon (M1/M2/M3), Linux ARM64, Windows ARM64
 * 
 * ARM64 Calling Convention:
 *   - x0-x7:  argument registers (x0 also return value)
 *   - x8:     indirect result location register
 *   - x9-x15: temporary registers (caller-saved)
 *   - x16-x17: intra-procedure-call scratch (IP0/IP1)
 *   - x18:    platform register (reserved on Apple)
 *   - x19-x28: callee-saved registers
 *   - x29:    frame pointer (FP)
 *   - x30:    link register (LR)
 *   - sp:     stack pointer
 *   - d0-d7:  floating-point argument/return registers
 *   - d8-d15: callee-saved floating-point
 *   - d16-d31: caller-saved floating-point
 * 
 * Our JIT convention:
 *   - x0: stack pointer (JITValue*)
 *   - x1: locals pointer (JITValue*)
 *   - x2: runtime pointer (void*)
 *   - x9-x15: scratch registers for computations
 */

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Hole Markers - Magic values replaced at runtime
// =============================================================================
// These are defined via compiler -D flags during stencil compilation

volatile uint64_t HOLE_imm64 = 0xDEADBEEFCAFEBABEULL;
volatile uint64_t HOLE_stack_ptr = 0xAAAAAAAAAAAAAAAAULL;
volatile uint64_t HOLE_jump_target = 0xBBBBBBBBBBBBBBBBULL;
volatile uint64_t HOLE_slot_idx = 0xCCCCCCCCCCCCCCCCULL;
volatile uint64_t HOLE_imm64_2 = 0xDDDDDDDDDDDDDDDDULL;
volatile uint64_t HOLE_func_ptr = 0xEEEEEEEEEEEEEEEEULL;

// =============================================================================
// JITValue Layout (16 bytes, matches jit.h)
// =============================================================================
// Offset 0:  uint64_t bits (value payload)
// Offset 8:  uint8_t tag (type tag)
// Offset 9-15: padding

#define TAG_INT    0
#define TAG_DOUBLE 1
#define TAG_BOOL   2
#define TAG_STRING 3
#define TAG_ARRAY  4
#define TAG_DICT   5
#define TAG_NIL    6

#define JITVALUE_SIZE 16

// Prevent inlining and optimization to preserve stencil boundaries
#define STENCIL __attribute__((noinline, used, optnone))

// =============================================================================
// Push Operations
// =============================================================================

STENCIL
void stencil_push_int32(void) {
    // Push 32-bit integer (sign-extended to 64-bit)
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t value = (int64_t)(int32_t)HOLE_imm64;
    stack[0] = (uint64_t)value;  // bits
    *((uint8_t*)&stack[1]) = TAG_INT;  // tag at offset 8
}

STENCIL
void stencil_push_int64(void) {
    // Push 64-bit integer
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = HOLE_imm64;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

STENCIL
void stencil_push_double(void) {
    // Push double (bits passed directly)
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = HOLE_imm64;
    *((uint8_t*)&stack[1]) = TAG_DOUBLE;
}

STENCIL
void stencil_push_true(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = 1;
    *((uint8_t*)&stack[1]) = TAG_BOOL;
}

STENCIL
void stencil_push_false(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = 0;
    *((uint8_t*)&stack[1]) = TAG_BOOL;
}

STENCIL
void stencil_push_int_0(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = 0;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

STENCIL
void stencil_push_int_1(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = 1;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

STENCIL
void stencil_push_int_neg1(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = (uint64_t)-1LL;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

STENCIL
void stencil_push_nil(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = 0;
    *((uint8_t*)&stack[1]) = TAG_NIL;
}

// =============================================================================
// Pop Operation
// =============================================================================

STENCIL
void stencil_pop(void) {
    // Pop is handled by stack pointer adjustment (no code needed)
    // This stencil exists as a placeholder for stack tracking
    __asm__ volatile("nop");
}

STENCIL
void stencil_pop2(void) {
    // Pop two values
    __asm__ volatile("nop; nop");
}

// =============================================================================
// Local Variable Operations
// =============================================================================

STENCIL
void stencil_load_slot(void) {
    // Load from locals[slot] to stack
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t slot = HOLE_slot_idx;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);  // Locals below stack
    uint64_t* src = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    stack[0] = src[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&src[1]);
}

STENCIL
void stencil_store_slot(void) {
    // Store from stack to locals[slot]
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t slot = HOLE_slot_idx;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* dst = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    dst[0] = stack[0];
    *((uint8_t*)&dst[1]) = *((uint8_t*)&stack[1]);
}

STENCIL
void stencil_load_slot_0(void) {
    // Optimized: load locals[0]
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    stack[0] = locals[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&locals[1]);
}

STENCIL
void stencil_load_slot_1(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* src = (uint64_t*)((uint8_t*)locals + JITVALUE_SIZE);
    stack[0] = src[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&src[1]);
}

STENCIL
void stencil_load_slot_2(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* src = (uint64_t*)((uint8_t*)locals + 2 * JITVALUE_SIZE);
    stack[0] = src[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&src[1]);
}

STENCIL
void stencil_load_slot_3(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* src = (uint64_t*)((uint8_t*)locals + 3 * JITVALUE_SIZE);
    stack[0] = src[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&src[1]);
}

STENCIL
void stencil_store_slot_0(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    locals[0] = stack[0];
    *((uint8_t*)&locals[1]) = *((uint8_t*)&stack[1]);
}

STENCIL
void stencil_store_slot_1(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* dst = (uint64_t*)((uint8_t*)locals + JITVALUE_SIZE);
    dst[0] = stack[0];
    *((uint8_t*)&dst[1]) = *((uint8_t*)&stack[1]);
}

STENCIL
void stencil_store_slot_2(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* dst = (uint64_t*)((uint8_t*)locals + 2 * JITVALUE_SIZE);
    dst[0] = stack[0];
    *((uint8_t*)&dst[1]) = *((uint8_t*)&stack[1]);
}

STENCIL
void stencil_store_slot_3(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* dst = (uint64_t*)((uint8_t*)locals + 3 * JITVALUE_SIZE);
    dst[0] = stack[0];
    *((uint8_t*)&dst[1]) = *((uint8_t*)&stack[1]);
}

// =============================================================================
// Integer Arithmetic (assumes TAG_INT)
// =============================================================================

STENCIL
void stencil_add_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];  // TOS
    int64_t a = (int64_t)stack[-2]; // TOS-1 (accounting for 16-byte values)
    stack[-2] = (uint64_t)(a + b);
    // Tag already set at stack[-2]
}

STENCIL
void stencil_sub_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (uint64_t)(a - b);
}

STENCIL
void stencil_mul_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (uint64_t)(a * b);
}

STENCIL
void stencil_div_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    if (b != 0) {
        stack[-2] = (uint64_t)(a / b);
    }
}

STENCIL
void stencil_mod_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    if (b != 0) {
        stack[-2] = (uint64_t)(a % b);
    }
}

STENCIL
void stencil_neg_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t a = (int64_t)stack[0];
    stack[0] = (uint64_t)(-a);
}

STENCIL
void stencil_inc_slot(void) {
    // Increment local variable in place
    uint64_t slot = HOLE_slot_idx;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    val[0] = val[0] + 1;
}

STENCIL
void stencil_dec_slot(void) {
    uint64_t slot = HOLE_slot_idx;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    val[0] = val[0] - 1;
}

STENCIL
void stencil_add_int_imm(void) {
    // Add immediate to TOS
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t imm = (int64_t)HOLE_imm64;
    stack[0] = (uint64_t)((int64_t)stack[0] + imm);
}

STENCIL
void stencil_inc_slot_0(void) {
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    locals[0] = locals[0] + 1;
}

STENCIL
void stencil_inc_slot_1(void) {
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + JITVALUE_SIZE);
    val[0] = val[0] + 1;
}

// =============================================================================
// Double Arithmetic
// =============================================================================

STENCIL
void stencil_add_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b, r;
    b.u = stack[0];
    a.u = stack[-2];
    r.d = a.d + b.d;
    stack[-2] = r.u;
}

STENCIL
void stencil_sub_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b, r;
    b.u = stack[0];
    a.u = stack[-2];
    r.d = a.d - b.d;
    stack[-2] = r.u;
}

STENCIL
void stencil_mul_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b, r;
    b.u = stack[0];
    a.u = stack[-2];
    r.d = a.d * b.d;
    stack[-2] = r.u;
}

STENCIL
void stencil_div_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b, r;
    b.u = stack[0];
    a.u = stack[-2];
    r.d = a.d / b.d;
    stack[-2] = r.u;
}

STENCIL
void stencil_neg_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } v;
    v.u = stack[0];
    v.d = -v.d;
    stack[0] = v.u;
}

// =============================================================================
// Integer Comparisons
// =============================================================================

STENCIL
void stencil_eq_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a == b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_ne_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a != b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_lt_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a < b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_le_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a <= b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_gt_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a > b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_ge_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    stack[-2] = (a >= b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

// =============================================================================
// Double Comparisons
// =============================================================================

STENCIL
void stencil_eq_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b;
    b.u = stack[0];
    a.u = stack[-2];
    stack[-2] = (a.d == b.d) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_lt_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b;
    b.u = stack[0];
    a.u = stack[-2];
    stack[-2] = (a.d < b.d) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_le_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b;
    b.u = stack[0];
    a.u = stack[-2];
    stack[-2] = (a.d <= b.d) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_gt_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b;
    b.u = stack[0];
    a.u = stack[-2];
    stack[-2] = (a.d > b.d) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_ge_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } a, b;
    b.u = stack[0];
    a.u = stack[-2];
    stack[-2] = (a.d >= b.d) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

// =============================================================================
// Logical Operations
// =============================================================================

STENCIL
void stencil_not(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t val = stack[0];
    uint8_t tag = *((uint8_t*)&stack[1]);
    bool truthy;
    if (tag == TAG_BOOL) {
        truthy = (val != 0);
    } else if (tag == TAG_INT) {
        truthy = (val != 0);
    } else {
        truthy = true;  // Non-nil values are truthy
    }
    stack[0] = truthy ? 0 : 1;
    *((uint8_t*)&stack[1]) = TAG_BOOL;
}

STENCIL
void stencil_and(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t b = stack[0];
    uint64_t a = stack[-2];
    stack[-2] = (a && b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

STENCIL
void stencil_or(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t b = stack[0];
    uint64_t a = stack[-2];
    stack[-2] = (a || b) ? 1 : 0;
    *((uint8_t*)&stack[-1]) = TAG_BOOL;
}

// =============================================================================
// Bitwise Operations
// =============================================================================

STENCIL
void stencil_bitand(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[-2] = stack[-2] & stack[0];
}

STENCIL
void stencil_bitor(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[-2] = stack[-2] | stack[0];
}

STENCIL
void stencil_bitxor(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[-2] = stack[-2] ^ stack[0];
}

STENCIL
void stencil_bitnot(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = ~stack[0];
}

STENCIL
void stencil_shl(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t shift = stack[0] & 63;
    stack[-2] = stack[-2] << shift;
}

STENCIL
void stencil_shr(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t shift = stack[0] & 63;
    stack[-2] = (int64_t)stack[-2] >> shift;  // Arithmetic shift
}

STENCIL
void stencil_ushr(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t shift = stack[0] & 63;
    stack[-2] = stack[-2] >> shift;  // Logical shift
}

// =============================================================================
// Control Flow
// =============================================================================

STENCIL
void stencil_jump(void) {
    // Unconditional jump - target patched at runtime
    volatile void* target = (void*)HOLE_jump_target;
    __asm__ volatile("br %0" : : "r"(target));
}

STENCIL
void stencil_jump_if_false(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t val = stack[0];
    volatile void* target = (void*)HOLE_jump_target;
    if (val == 0) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_jump_if_true(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t val = stack[0];
    volatile void* target = (void*)HOLE_jump_target;
    if (val != 0) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_jump_if_zero(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t val = (int64_t)stack[0];
    volatile void* target = (void*)HOLE_jump_target;
    if (val == 0) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_jump_if_nonzero(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t val = (int64_t)stack[0];
    volatile void* target = (void*)HOLE_jump_target;
    if (val != 0) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

// =============================================================================
// Fused Operations (Performance Critical)
// =============================================================================

STENCIL
void stencil_load_slot_push_int(void) {
    // Fused: load local, push immediate
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t slot = HOLE_slot_idx;
    int64_t imm = (int64_t)HOLE_imm64_2;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* src = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    // Copy local to stack[0]
    stack[0] = src[0];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&src[1]);
    
    // Push immediate to stack[2] (next slot)
    stack[2] = (uint64_t)imm;
    *((uint8_t*)&stack[3]) = TAG_INT;
}

STENCIL
void stencil_add_store_slot(void) {
    // Fused: add TOS-1 + TOS, store to slot
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t slot = HOLE_slot_idx;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* dst = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    dst[0] = (uint64_t)(a + b);
    *((uint8_t*)&dst[1]) = TAG_INT;
}

STENCIL
void stencil_loop_cond_lt_int(void) {
    // Optimized loop: if (slot < limit) goto target
    uint64_t slot = HOLE_slot_idx;
    int64_t limit = (int64_t)HOLE_imm64;
    volatile void* target = (void*)HOLE_jump_target;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    if ((int64_t)val[0] < limit) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_loop_cond_le_int(void) {
    uint64_t slot = HOLE_slot_idx;
    int64_t limit = (int64_t)HOLE_imm64;
    volatile void* target = (void*)HOLE_jump_target;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    if ((int64_t)val[0] <= limit) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_inc_and_loop(void) {
    // Fused: increment slot, if < limit goto target
    uint64_t slot = HOLE_slot_idx;
    int64_t limit = (int64_t)HOLE_imm64;
    volatile void* target = (void*)HOLE_jump_target;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    val[0] = val[0] + 1;
    if ((int64_t)val[0] < limit) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_dec_and_loop(void) {
    // Fused: decrement slot, if > 0 goto target
    uint64_t slot = HOLE_slot_idx;
    volatile void* target = (void*)HOLE_jump_target;
    volatile uint64_t* locals = (uint64_t*)(HOLE_stack_ptr - 0x1000);
    uint64_t* val = (uint64_t*)((uint8_t*)locals + slot * JITVALUE_SIZE);
    
    val[0] = val[0] - 1;
    if ((int64_t)val[0] > 0) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

STENCIL
void stencil_compare_and_jump(void) {
    // Compare TOS-1 < TOS and jump if true
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    volatile void* target = (void*)HOLE_jump_target;
    int64_t b = (int64_t)stack[0];
    int64_t a = (int64_t)stack[-2];
    
    if (a < b) {
        __asm__ volatile("br %0" : : "r"(target));
    }
}

// =============================================================================
// Function Call Support
// =============================================================================

STENCIL
void stencil_call_runtime(void) {
    // Call a runtime function (function pointer in HOLE_func_ptr)
    typedef void (*RuntimeFn)(void* runtime);
    RuntimeFn fn = (RuntimeFn)HOLE_func_ptr;
    void* runtime = (void*)HOLE_stack_ptr;  // Runtime ptr passed via stack
    fn(runtime);
}

STENCIL
void stencil_return(void) {
    // Return from JIT'd function
    __asm__ volatile("ret");
}

// =============================================================================
// Stack Manipulation
// =============================================================================

STENCIL
void stencil_dup(void) {
    // Duplicate TOS
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[2] = stack[0];
    *((uint8_t*)&stack[3]) = *((uint8_t*)&stack[1]);
}

STENCIL
void stencil_dup2(void) {
    // Duplicate TOS and TOS-1
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[4] = stack[0];
    *((uint8_t*)&stack[5]) = *((uint8_t*)&stack[1]);
    stack[2] = stack[-2];
    *((uint8_t*)&stack[3]) = *((uint8_t*)&stack[-1]);
}

STENCIL
void stencil_swap(void) {
    // Swap TOS and TOS-1
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t tmp_bits = stack[0];
    uint8_t tmp_tag = *((uint8_t*)&stack[1]);
    
    stack[0] = stack[-2];
    *((uint8_t*)&stack[1]) = *((uint8_t*)&stack[-1]);
    
    stack[-2] = tmp_bits;
    *((uint8_t*)&stack[-1]) = tmp_tag;
}

STENCIL
void stencil_rot3(void) {
    // Rotate top 3: [a, b, c] -> [b, c, a]
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    uint64_t a_bits = stack[-4];
    uint8_t a_tag = *((uint8_t*)&stack[-3]);
    
    stack[-4] = stack[-2];
    *((uint8_t*)&stack[-3]) = *((uint8_t*)&stack[-1]);
    
    stack[-2] = stack[0];
    *((uint8_t*)&stack[-1]) = *((uint8_t*)&stack[1]);
    
    stack[0] = a_bits;
    *((uint8_t*)&stack[1]) = a_tag;
}

// =============================================================================
// Type Conversions
// =============================================================================

STENCIL
void stencil_int_to_double(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t i = (int64_t)stack[0];
    union { uint64_t u; double d; } conv;
    conv.d = (double)i;
    stack[0] = conv.u;
    *((uint8_t*)&stack[1]) = TAG_DOUBLE;
}

STENCIL
void stencil_double_to_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    union { uint64_t u; double d; } conv;
    conv.u = stack[0];
    stack[0] = (uint64_t)(int64_t)conv.d;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

STENCIL
void stencil_bool_to_int(void) {
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    stack[0] = (stack[0] != 0) ? 1 : 0;
    *((uint8_t*)&stack[1]) = TAG_INT;
}

// =============================================================================
// Range Check (for array bounds)
// =============================================================================

STENCIL
void stencil_check_bounds(void) {
    // Check if index (TOS) is in range [0, limit)
    volatile uint64_t* stack = (uint64_t*)HOLE_stack_ptr;
    int64_t index = (int64_t)stack[0];
    int64_t limit = (int64_t)HOLE_imm64;
    
    if (index < 0 || index >= limit) {
        // Set error flag (would need runtime support)
        stack[0] = (uint64_t)-1;
    }
}

// =============================================================================
// No-op (for alignment/padding)
// =============================================================================

STENCIL
void stencil_nop(void) {
    __asm__ volatile("nop");
}

STENCIL
void stencil_nop2(void) {
    __asm__ volatile("nop; nop");
}

STENCIL
void stencil_nop4(void) {
    __asm__ volatile("nop; nop; nop; nop");
}
