/**
 * =============================================================================
 * Quartz JIT Stencils - x86_64 Copy-and-Patch Code Templates
 * =============================================================================
 *
 * These are machine code templates compiled at build time. At runtime, the JIT
 * copies these templates and patches in concrete values (constants, addresses).
 *
 * CRITICAL OPTIMIZATIONS:
 * 1. Each stencil is standalone - no function call overhead
 * 2. "Holes" use 64-bit magic values that are easy to find and patch
 * 3. Compiled with -O2 for good codegen without excessive inlining
 * 4. No stack protector, no exceptions, minimal prologue/epilogue
 * 5. Uses x86_64-specific optimizations (LEA, CMOV, etc.)
 *
 * The stencil compiler extracts the raw bytes and relocation info.
 * 
 * x86_64 Calling Convention (System V AMD64 ABI):
 *   - rdi, rsi, rdx, rcx, r8, r9: integer arguments
 *   - xmm0-xmm7: floating-point arguments  
 *   - rax: return value
 *   - rbx, rbp, r12-r15: callee-saved
 *   - rsp: stack pointer (16-byte aligned before call)
 *
 * Our JIT convention:
 *   - rdi: stack pointer (JITValue*)
 *   - rsi: locals pointer (JITValue*)
 *   - rdx: runtime pointer (void*)
 */

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Hole Markers (Magic Values)
// =============================================================================

#ifndef HOLE_IMM64
#define HOLE_IMM64 0xDEADBEEFCAFEBABEULL
#endif

#ifndef HOLE_STACK_PTR  
#define HOLE_STACK_PTR 0xAAAAAAAAAAAAAAAAULL
#endif

#ifndef HOLE_JUMP_TARGET
#define HOLE_JUMP_TARGET 0xBBBBBBBBBBBBBBBBULL
#endif

#ifndef HOLE_SLOT_IDX
#define HOLE_SLOT_IDX 0xCCCCCCCCCCCCCCCCULL
#endif

#ifndef HOLE_IMM64_2
#define HOLE_IMM64_2 0xDDDDDDDDDDDDDDDDULL
#endif

#ifndef HOLE_FUNC_PTR
#define HOLE_FUNC_PTR 0xEEEEEEEEEEEEEEEEULL
#endif

// Volatile global variables - compiler will load these, we patch the loads
volatile uint64_t HOLE_imm64 = HOLE_IMM64;
volatile uint64_t HOLE_stack_ptr = HOLE_STACK_PTR;
volatile uint64_t HOLE_jump_target = HOLE_JUMP_TARGET;
volatile uint64_t HOLE_slot_idx = HOLE_SLOT_IDX;
volatile uint64_t HOLE_imm64_2 = HOLE_IMM64_2;
volatile uint64_t HOLE_func_ptr = HOLE_FUNC_PTR;

// =============================================================================
// Value Layout (matches Quartz JITValue type - 16 bytes)
// =============================================================================

typedef struct {
    uint64_t bits;      // Raw bits (int64, double bits, or pointer)
    uint8_t  tag;       // 0=int, 1=double, 2=bool, 3=string_ptr, etc.
    uint8_t  _pad[7];   // Alignment padding to 16 bytes
} JITValue;

#define TAG_INT    0
#define TAG_DOUBLE 1
#define TAG_BOOL   2
#define TAG_STRING 3
#define TAG_ARRAY  4
#define TAG_DICT   5
#define TAG_NIL    6

#define JITVALUE_SIZE 16

static inline int64_t jv_as_int(JITValue v) { return (int64_t)v.bits; }
static inline double jv_as_double(JITValue v) { return *(double*)&v.bits; }
static inline bool jv_as_bool(JITValue v) { return v.bits != 0; }

static inline JITValue jv_make_int(int64_t x) {
    JITValue v = { .bits = (uint64_t)x, .tag = TAG_INT };
    return v;
}

static inline JITValue jv_make_double(double x) {
    JITValue v;
    v.tag = TAG_DOUBLE;
    *(double*)&v.bits = x;
    return v;
}

static inline JITValue jv_make_bool(bool x) {
    JITValue v = { .bits = x ? 1 : 0, .tag = TAG_BOOL };
    return v;
}

// =============================================================================
// Stencil Declarations
// =============================================================================

#define STENCIL __attribute__((noinline, used))

// =============================================================================
// Stack Manipulation Stencils
// =============================================================================

/**
 * Push 32-bit integer constant onto stack
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_push_int32(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int32_t imm = (int32_t)HOLE_IMM64;
    stack[0] = jv_make_int(imm);
}

/**
 * Push 64-bit double constant onto stack
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_push_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t bits = HOLE_IMM64;
    JITValue v;
    v.bits = bits;
    v.tag = TAG_DOUBLE;
    stack[0] = v;
}

/**
 * Push boolean true
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_true(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool(1);
}

/**
 * Push boolean false
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_false(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool(0);
}

/**
 * Push integer 0 (optimized, common case)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_int_0(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int(0);
}

/**
 * Push integer 1 (optimized, common case)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_int_1(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int(1);
}

/**
 * Push integer -1 (optimized, common case)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_int_neg1(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int(-1);
}

/**
 * Pop top of stack (no-op stencil)
 */
STENCIL
void stencil_pop(void) {
    __asm__ volatile("nop");
}

// =============================================================================
// Local Variable Stencils (Slot-Based)
// =============================================================================

/**
 * Load from local slot onto stack
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_load_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    JITValue* locals = stack;
    stack[0] = locals[-(int64_t)slot_idx - 1];
}

/**
 * Store to local slot from stack top
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_store_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    JITValue* locals = stack;
    locals[-(int64_t)slot_idx - 1] = stack[0];
}

/**
 * Load from slot 0 (most common local, optimized)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_load_slot_0(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = stack[-1];
}

/**
 * Store to slot 0 (most common local, optimized)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_store_slot_0(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1] = stack[0];
}

// =============================================================================
// Integer Arithmetic Stencils (Fast Path)
// =============================================================================

/**
 * Integer addition: stack[-1] = stack[-1] + stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_add_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    stack[-1] = jv_make_int(a + b);
}

/**
 * Integer subtraction: stack[-1] = stack[-1] - stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_sub_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    stack[-1] = jv_make_int(a - b);
}

/**
 * Integer multiplication: stack[-1] = stack[-1] * stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_mul_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    stack[-1] = jv_make_int(a * b);
}

/**
 * Integer division: stack[-1] = stack[-1] / stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_div_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    stack[-1] = jv_make_int(a / b);
}

/**
 * Integer modulo: stack[-1] = stack[-1] % stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_mod_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    stack[-1] = jv_make_int(a % b);
}

/**
 * Integer negation: stack[0] = -stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_neg_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int(-jv_as_int(stack[0]));
}

/**
 * Increment slot: locals[slot]++
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_inc_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    JITValue* locals = stack;
    int64_t val = jv_as_int(locals[-(int64_t)slot_idx - 1]);
    locals[-(int64_t)slot_idx - 1] = jv_make_int(val + 1);
}

/**
 * Decrement slot: locals[slot]--
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_dec_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    JITValue* locals = stack;
    int64_t val = jv_as_int(locals[-(int64_t)slot_idx - 1]);
    locals[-(int64_t)slot_idx - 1] = jv_make_int(val - 1);
}

// =============================================================================
// Double Arithmetic Stencils
// =============================================================================

/**
 * Double addition: stack[-1] = stack[-1] + stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_add_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    double a = jv_as_double(stack[-1]);
    double b = jv_as_double(stack[0]);
    stack[-1] = jv_make_double(a + b);
}

/**
 * Double subtraction: stack[-1] = stack[-1] - stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_sub_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    double a = jv_as_double(stack[-1]);
    double b = jv_as_double(stack[0]);
    stack[-1] = jv_make_double(a - b);
}

/**
 * Double multiplication: stack[-1] = stack[-1] * stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_mul_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    double a = jv_as_double(stack[-1]);
    double b = jv_as_double(stack[0]);
    stack[-1] = jv_make_double(a * b);
}

/**
 * Double division: stack[-1] = stack[-1] / stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_div_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    double a = jv_as_double(stack[-1]);
    double b = jv_as_double(stack[0]);
    stack[-1] = jv_make_double(a / b);
}

/**
 * Double negation: stack[0] = -stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_neg_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_double(-jv_as_double(stack[0]));
}

// =============================================================================
// Comparison Stencils (Integer)
// =============================================================================

/**
 * Integer equal: stack[-1] = (stack[-1] == stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_eq_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) == jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Integer not equal: stack[-1] = (stack[-1] != stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_ne_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) != jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Integer less than: stack[-1] = (stack[-1] < stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_lt_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) < jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Integer greater than: stack[-1] = (stack[-1] > stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_gt_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) > jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Integer less than or equal: stack[-1] = (stack[-1] <= stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_le_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) <= jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Integer greater than or equal: stack[-1] = (stack[-1] >= stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_ge_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_int(stack[-1]) >= jv_as_int(stack[0]);
    stack[-1] = jv_make_bool(result);
}

// =============================================================================
// Comparison Stencils (Double)
// =============================================================================

/**
 * Double equal: stack[-1] = (stack[-1] == stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_eq_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_double(stack[-1]) == jv_as_double(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Double less than: stack[-1] = (stack[-1] < stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_lt_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_double(stack[-1]) < jv_as_double(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Double greater than: stack[-1] = (stack[-1] > stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_gt_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_double(stack[-1]) > jv_as_double(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Double less or equal: stack[-1] = (stack[-1] <= stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_le_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_double(stack[-1]) <= jv_as_double(stack[0]);
    stack[-1] = jv_make_bool(result);
}

/**
 * Double greater or equal: stack[-1] = (stack[-1] >= stack[0])
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_ge_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool result = jv_as_double(stack[-1]) >= jv_as_double(stack[0]);
    stack[-1] = jv_make_bool(result);
}

// =============================================================================
// Logical / Boolean Stencils
// =============================================================================

/**
 * Logical NOT: stack[0] = !stack[0]
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_not(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    bool val;
    if (stack[0].tag == TAG_BOOL) {
        val = jv_as_bool(stack[0]);
    } else if (stack[0].tag == TAG_INT) {
        val = jv_as_int(stack[0]) != 0;
    } else {
        val = true;
    }
    stack[0] = jv_make_bool(!val);
}

// =============================================================================
// Control Flow Stencils
// =============================================================================

/**
 * Unconditional jump (patched address)
 * Holes: HOLE_JUMP_TARGET
 */
STENCIL
void stencil_jump(void) {
    void* target = (void*)HOLE_JUMP_TARGET;
    __asm__ volatile("jmp *%0" :: "r"(target));
}

/**
 * Jump if top of stack is false
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_jump_if_false(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;

    bool condition;
    if (stack[0].tag == TAG_BOOL) {
        condition = jv_as_bool(stack[0]);
    } else if (stack[0].tag == TAG_INT) {
        condition = jv_as_int(stack[0]) != 0;
    } else {
        condition = true;
    }

    if (!condition) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Jump if top of stack is true
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_jump_if_true(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;

    bool condition;
    if (stack[0].tag == TAG_BOOL) {
        condition = jv_as_bool(stack[0]);
    } else if (stack[0].tag == TAG_INT) {
        condition = jv_as_int(stack[0]) != 0;
    } else {
        condition = true;
    }

    if (condition) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

// =============================================================================
// Super-Instructions (Fused Common Patterns)
// =============================================================================

/**
 * Load slot and push constant
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64
 */
STENCIL
void stencil_load_slot_push_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t imm = (int64_t)HOLE_IMM64;

    JITValue* locals = stack;
    stack[0] = locals[-(int64_t)slot_idx - 1];
    stack[1] = jv_make_int(imm);
}

/**
 * Binary add and store to slot
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_add_store_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;

    int64_t a = jv_as_int(stack[-1]);
    int64_t b = jv_as_int(stack[0]);
    JITValue* locals = stack;
    locals[-(int64_t)slot_idx - 1] = jv_make_int(a + b);
}

/**
 * Loop condition: if slot < limit then continue else jump
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_loop_cond_lt_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t limit = (int64_t)HOLE_IMM64;
    void* target = (void*)HOLE_JUMP_TARGET;

    JITValue* locals = stack;
    int64_t counter = jv_as_int(locals[-(int64_t)slot_idx - 1]);

    if (counter >= limit) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Increment and loop pattern
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_inc_and_loop(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t limit = (int64_t)HOLE_IMM64;
    void* target = (void*)HOLE_JUMP_TARGET;

    JITValue* locals = stack;
    int64_t counter = jv_as_int(locals[-(int64_t)slot_idx - 1]) + 1;
    locals[-(int64_t)slot_idx - 1] = jv_make_int(counter);

    if (counter < limit) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

// =============================================================================
// Runtime Call Stencils
// =============================================================================

/**
 * Call a runtime function
 * Holes: HOLE_FUNC_PTR
 */
STENCIL
void stencil_call_runtime(void) {
    void (*func)(void) = (void (*)(void))HOLE_FUNC_PTR;
    func();
}

/**
 * Return from JIT code
 */
STENCIL
void stencil_return(void) {
    __asm__ volatile("ret");
}

// =============================================================================
// Duplicate and Swap
// =============================================================================

/**
 * Duplicate top of stack
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_dup(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[1] = stack[0];
}

/**
 * Swap top two stack values
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_swap(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    JITValue tmp = stack[0];
    stack[0] = stack[-1];
    stack[-1] = tmp;
}

// =============================================================================
// Additional Optimized Slot Operations (x86_64)
// =============================================================================

/**
 * Load slot 1
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_load_slot_1(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = stack[-2];  // slot 1
}

/**
 * Load slot 2
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_load_slot_2(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = stack[-3];  // slot 2
}

/**
 * Load slot 3
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_load_slot_3(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = stack[-4];  // slot 3
}

/**
 * Store to slot 1
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_store_slot_1(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-2] = stack[0];
}

/**
 * Store to slot 2
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_store_slot_2(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-3] = stack[0];
}

/**
 * Store to slot 3
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_store_slot_3(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-4] = stack[0];
}

/**
 * Increment slot 0 in place
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_inc_slot_0(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits++;
}

/**
 * Increment slot 1 in place
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_inc_slot_1(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-2].bits++;
}

/**
 * Decrement slot 0 in place
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_dec_slot_0(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits--;
}

// =============================================================================
// Additional Arithmetic Operations
// =============================================================================

/**
 * Push int64 (full 64-bit)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_push_int64(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int((int64_t)HOLE_IMM64);
}

/**
 * Add immediate to TOS
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_add_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0].bits += HOLE_IMM64;
}

/**
 * Subtract immediate from TOS
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_sub_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0].bits -= HOLE_IMM64;
}

/**
 * Multiply TOS by immediate
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_mul_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0].bits = (uint64_t)((int64_t)stack[0].bits * (int64_t)HOLE_IMM64);
}

/**
 * Push nil value
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_push_nil(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0].bits = 0;
    stack[0].tag = TAG_NIL;
}

// =============================================================================
// Bitwise Operations
// =============================================================================

/**
 * Bitwise AND
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_bitand(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits &= stack[0].bits;
}

/**
 * Bitwise OR
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_bitor(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits |= stack[0].bits;
}

/**
 * Bitwise XOR
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_bitxor(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits ^= stack[0].bits;
}

/**
 * Bitwise NOT
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_bitnot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0].bits = ~stack[0].bits;
}

/**
 * Left shift
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_shl(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits <<= (stack[0].bits & 63);
}

/**
 * Right shift (arithmetic)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_shr(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits = (uint64_t)((int64_t)stack[-1].bits >> (stack[0].bits & 63));
}

/**
 * Right shift (logical)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_ushr(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1].bits >>= (stack[0].bits & 63);
}

// =============================================================================
// Logical Operations
// =============================================================================

/**
 * Logical AND
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_and(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1] = jv_make_bool(stack[-1].bits && stack[0].bits);
}

/**
 * Logical OR
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_or(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[-1] = jv_make_bool(stack[-1].bits || stack[0].bits);
}

// =============================================================================
// Additional Comparison Operations
// =============================================================================

/**
 * Compare TOS with immediate (less than)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_lt_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool((int64_t)stack[0].bits < (int64_t)HOLE_IMM64);
}

/**
 * Compare TOS with immediate (less than or equal)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_le_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool((int64_t)stack[0].bits <= (int64_t)HOLE_IMM64);
}

/**
 * Compare TOS with immediate (greater than)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_gt_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool((int64_t)stack[0].bits > (int64_t)HOLE_IMM64);
}

/**
 * Compare TOS with immediate (equal)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_eq_int_imm(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool(stack[0].bits == HOLE_IMM64);
}

/**
 * Compare TOS with zero
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_is_zero(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool(stack[0].bits == 0);
}

/**
 * Compare TOS with zero (not equal)
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_is_nonzero(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_bool(stack[0].bits != 0);
}

// =============================================================================
// Additional Control Flow
// =============================================================================

/**
 * Jump if TOS is zero
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_jump_if_zero(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;
    if (stack[0].bits == 0) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Jump if TOS is nonzero
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_jump_if_nonzero(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;
    if (stack[0].bits != 0) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Loop condition: slot <= limit
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_loop_cond_le_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t limit = (int64_t)HOLE_IMM64;
    void* target = (void*)HOLE_JUMP_TARGET;
    
    JITValue* locals = stack;
    int64_t counter = jv_as_int(locals[-(int64_t)slot_idx - 1]);
    
    if (counter > limit) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Decrement and loop (while > 0)
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_dec_and_loop(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    void* target = (void*)HOLE_JUMP_TARGET;
    
    JITValue* locals = stack;
    int64_t counter = jv_as_int(locals[-(int64_t)slot_idx - 1]) - 1;
    locals[-(int64_t)slot_idx - 1] = jv_make_int(counter);
    
    if (counter > 0) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Compare and jump if less than
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_compare_and_jump_lt(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;
    if ((int64_t)stack[-1].bits < (int64_t)stack[0].bits) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Compare and jump if equal
 * Holes: HOLE_STACK_PTR, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_compare_and_jump_eq(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    void* target = (void*)HOLE_JUMP_TARGET;
    if (stack[-1].bits == stack[0].bits) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

// =============================================================================
// Stack Manipulation (Advanced)
// =============================================================================

/**
 * Duplicate second element
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_dup2(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[1] = stack[-1];
    stack[2] = stack[0];
}

/**
 * Rotate top 3 elements
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_rot3(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    JITValue tmp = stack[-2];
    stack[-2] = stack[-1];
    stack[-1] = stack[0];
    stack[0] = tmp;
}

/**
 * Pop 2 elements
 * Holes: none (just for tracking)
 */
STENCIL
void stencil_pop2(void) {
    __asm__ volatile("nop; nop");
}

/**
 * Over: copy second element to top
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_over(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[1] = stack[-1];
}

// =============================================================================
// Type Conversions
// =============================================================================

/**
 * Convert int to double
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_int_to_double(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t i = (int64_t)stack[0].bits;
    stack[0] = jv_make_double((double)i);
}

/**
 * Convert double to int
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_double_to_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    double d = jv_as_double(stack[0]);
    stack[0] = jv_make_int((int64_t)d);
}

/**
 * Convert bool to int
 * Holes: HOLE_STACK_PTR
 */
STENCIL
void stencil_bool_to_int(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    stack[0] = jv_make_int(stack[0].bits ? 1 : 0);
}

// =============================================================================
// Fused Super-Instructions (High Performance)
// =============================================================================

/**
 * Load slot, compare with immediate, jump if true
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64, HOLE_JUMP_TARGET
 */
STENCIL
void stencil_load_cmp_lt_jump(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t limit = (int64_t)HOLE_IMM64;
    void* target = (void*)HOLE_JUMP_TARGET;
    
    JITValue* locals = stack;
    int64_t val = jv_as_int(locals[-(int64_t)slot_idx - 1]);
    
    if (val < limit) {
        __asm__ volatile("jmp *%0" :: "r"(target));
    }
}

/**
 * Add immediate to slot in place
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64
 */
STENCIL
void stencil_add_imm_to_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t imm = (int64_t)HOLE_IMM64;
    
    JITValue* locals = stack;
    locals[-(int64_t)slot_idx - 1].bits += imm;
}

/**
 * Subtract immediate from slot in place
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64
 */
STENCIL
void stencil_sub_imm_from_slot(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    int64_t imm = (int64_t)HOLE_IMM64;
    
    JITValue* locals = stack;
    locals[-(int64_t)slot_idx - 1].bits -= imm;
}

/**
 * Load two slots and add them
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX, HOLE_IMM64_2
 */
STENCIL
void stencil_load_two_add(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot1 = HOLE_SLOT_IDX;
    uint64_t slot2 = HOLE_IMM64_2;
    
    JITValue* locals = stack;
    int64_t a = jv_as_int(locals[-(int64_t)slot1 - 1]);
    int64_t b = jv_as_int(locals[-(int64_t)slot2 - 1]);
    stack[0] = jv_make_int(a + b);
}

/**
 * Store TOS to slot and pop
 * Holes: HOLE_STACK_PTR, HOLE_SLOT_IDX
 */
STENCIL
void stencil_store_and_pop(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    uint64_t slot_idx = HOLE_SLOT_IDX;
    JITValue* locals = stack;
    locals[-(int64_t)slot_idx - 1] = stack[0];
}

/**
 * Check bounds: if index (TOS) < 0 or >= limit, set error flag (-1)
 * Holes: HOLE_STACK_PTR, HOLE_IMM64
 */
STENCIL
void stencil_check_bounds(void) {
    JITValue* stack = (JITValue*)HOLE_STACK_PTR;
    int64_t index = (int64_t)stack[0].bits;
    int64_t limit = (int64_t)HOLE_imm64;
    if (index < 0 || index >= limit) {
        stack[0].bits = (uint64_t)-1;
        stack[0].tag = TAG_INT;
    }
}

/**
 * NOP placeholder
 */
STENCIL
void stencil_nop(void) {
    __asm__ volatile("nop");
}

/**
 * NOP x 4 (for alignment)
 */
STENCIL
void stencil_nop4(void) {
    __asm__ volatile("nop; nop; nop; nop");
}

// =============================================================================
// Marker for end of stencils
// =============================================================================
STENCIL
void stencil_end_marker(void) {
    __asm__ volatile(".byte 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC");
}
