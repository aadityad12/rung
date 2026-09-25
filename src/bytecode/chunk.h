#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/value.h"

namespace rung {

// Stack-VM bytecode (notes D3/D4: the register VM has its own, separate format). One function's
// code is a flat byte array: a one-byte opcode followed by its operands. Layout follows clox
// (Crafting Interpreters ch. 14-25) with these differences:
//   * constant indices, jump distances and the ARRAY element count are 3 bytes (24-bit,
//     big-endian), not clox's 1 and 2, so no program the resolver accepts overflows an operand
//     (notes D11: no engine may have a limit the others don't);
//   * NE, LE and GE are real opcodes, not `EQ NOT` etc., because `!(a < b)` is not `a >= b`
//     when an operand is NaN;
//   * there is no JUMP_IF_TRUE, and no global-name table: a global's operand is the constant
//     index of its interned name string, looked up in a hash table at run time (deliberately
//     slow: it is what inline caching, rung 3e, will remove).
//
// Operand widths: u8 = 1 byte (local slot, upvalue index, argument count: at most 255, which
// the resolver guarantees, notes D11); u24 = 3 bytes (constant index, jump distance, array
// length).
//
// Slot 0 of every frame holds the function being called, so the first parameter or local is
// slot 1 (as in clox). The comments say what each instruction does to the value stack.
enum class OpCode : std::uint8_t {
    // Constants and literals.
    Const,         // u24 index        push constants[index]
    Nil,           //                  push nil
    True,          //                  push true
    False,         //                  push false
    Pop,           //                  discard the top value
    // Variables.
    GetLocal,      // u8 slot          push stack[frame + slot]
    SetLocal,      // u8 slot          stack[frame + slot] = top (value stays on the stack)
    GetGlobal,     // u24 name index   push globals[constants[index]]; error if undefined
    SetGlobal,     // u24 name index   globals[name] = top; error if undefined; value stays
    DefineGlobal,  // u24 name index   globals[name] = pop (creates or overwrites)
    GetUpvalue,    // u8 index         push *closure.upvalues[index]
    SetUpvalue,    // u8 index         *closure.upvalues[index] = top (value stays)
    // Arithmetic, comparison, logic: pop the operand(s), push the result.
    Add, Sub, Mul, Div, Mod,
    Neg, Not,
    Eq, Ne, Lt, Le, Gt, Ge,
    // Control flow. A jump distance is measured from the byte after the instruction, so the
    // target of `Jump d` at offset o is o + 4 + d and of `Loop d` is o + 4 - d.
    Jump,          // u24 distance     ip += distance
    JumpIfFalse,   // u24 distance     if top is falsy, ip += distance. Does NOT pop: the
                   //                  compiler emits explicit Pops (`and` / `or` keep the value)
    Loop,          // u24 distance     ip -= distance
    // Functions.
    Call,          // u8 argc          callee is below its argc arguments on the stack
    Closure,       // u24 index, then for each upvalue of that function: u8 is_local, u8 index.
                   //                  push a closure of constants[index]. is_local = 1 captures
                   //                  the enclosing frame's local `index`; 0 copies the
                   //                  enclosing closure's upvalue `index`
    CloseUpvalue,  //                  close any open upvalue pointing at the top slot, then pop
    Return,        //                  pop the result, close the frame's upvalues, return it
    // Statements and arrays.
    Print,         //                  pop and print
    Array,         // u24 count        pop count values, push a new array of them (in order)
    IndexGet,      //                  pop index, pop array, push array[index]
    IndexSet,      //                  pop value, index, array; array[index] = value; push value
};

constexpr std::uint32_t kMaxU24 = 0xFFFFFFu;

inline void put_u24(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v);
}
inline std::uint32_t get_u24(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
           p[2];
}

// One function's code, constants and line table. Plain data: the compiler fills it in, the
// disassembler and the VM read it. Bytecode is immutable once compiled (notes D8).
struct Chunk {
    // The line table is run-length encoded: an entry says that code bytes from `offset` up to
    // the next entry's offset came from source line `line`.
    struct LineRun {
        std::size_t offset;
        int line;
    };

    std::vector<std::uint8_t> code;
    std::vector<Value> constants;
    std::vector<LineRun> lines;

    void emit_byte(std::uint8_t byte, int line);
    void emit_op(OpCode op, int line) { emit_byte(static_cast<std::uint8_t>(op), line); }
    void emit_u24(std::uint32_t value, int line);  // value must be <= kMaxU24

    // Appends without deduplicating; the caller checks the index against kMaxU24.
    std::size_t add_constant(Value value);

    // Source line of the code byte at `offset` (offset must be < code.size()).
    int line_at(std::size_t offset) const;

    // Bytes owned by the three vectors (capacity, like the heap's accounting).
    std::size_t owned_bytes() const;
};

}  // namespace rung
