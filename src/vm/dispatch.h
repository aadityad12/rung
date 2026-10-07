#pragma once

#include <cstdint>

#ifndef RUNG_COMPUTED_GOTO
#define RUNG_COMPUTED_GOTO 0
#endif

// Instruction dispatch for the bytecode VMs, behind macros so the loop body is the same for both
// implementations (notes D4), and both VMs (stack and register) use the same macros. The loop
// body uses only these macros and never writes `switch`, `case` or `goto` itself.
//
//   RUNG_OP_ENUM           defined by the VM before RUNG_DISPATCH: its opcode enum (`OpCode` or
//                          `RegOp`)
//   RUNG_OP_LIST(X)        defined by the VM before RUNG_DISPATCH: RUNG_OPCODE_LIST or
//                          RUNG_REG_OPCODE_LIST below, every enumerator of that enum in order
//   RUNG_FETCH()           defined by the VM before RUNG_DISPATCH: an expression that reads the
//                          next opcode (and advances the instruction pointer)
//   RUNG_DISPATCH()        starts the loop and dispatches the first instruction
//   RUNG_CASE(op)          the code of one opcode (the argument is an enumerator name)
//   RUNG_NEXT()            finish this instruction and dispatch the next one
//   RUNG_END_DISPATCH()    closes the loop opened by RUNG_DISPATCH
//
// Two implementations, chosen by the CMake option RUNG_COMPUTED_GOTO (ladder rung 3a):
//
// * switch (default): one shared indirect jump at the top of the loop. Every instruction ends by
//   jumping back to it, so the CPU's branch predictor sees one jump whose target depends on the
//   whole opcode stream.
// * computed goto: a table of label addresses indexed by opcode, and every handler ends with its
//   own `goto *table[next opcode]`. That is one indirect jump per handler, so the predictor keeps
//   a separate history for "what follows a GetLocal", "what follows an Add", and so on.
//   Labels-as-values are a GNU extension that clang supports.

// Every opcode, in the same order as the OpCode enum in bytecode/chunk.h (a static_assert in
// the VM checks the count). Only the computed-goto table needs it.
#define RUNG_OPCODE_LIST(X)                                                                    \
    X(Const) X(Nil) X(True) X(False) X(Pop)                                                    \
    X(GetLocal) X(SetLocal) X(GetGlobal) X(SetGlobal) X(DefineGlobal) X(GetUpvalue)            \
    X(SetUpvalue)                                                                              \
    X(Add) X(Sub) X(Mul) X(Div) X(Mod) X(Neg) X(Not)                                           \
    X(Eq) X(Ne) X(Lt) X(Le) X(Gt) X(Ge)                                                        \
    X(Jump) X(JumpIfFalse) X(Loop)                                                             \
    X(Call) X(Closure) X(CloseUpvalue) X(Return)                                               \
    X(Print) X(Array) X(IndexGet) X(IndexSet)

// The same for the register VM: every RegOp in bytecode/register_code.h, in enum order.
#define RUNG_REG_OPCODE_LIST(X)                                                                \
    X(Move) X(LoadK) X(LoadNil) X(LoadTrue) X(LoadFalse)                                       \
    X(GetGlobal) X(SetGlobal) X(DefineGlobal) X(GetUpvalue) X(SetUpvalue)                      \
    X(Add) X(Sub) X(Mul) X(Div) X(Mod)                                                         \
    X(Eq) X(Ne) X(Lt) X(Le) X(Gt) X(Ge) X(Neg) X(Not)                                          \
    X(Jump) X(JumpIfFalse) X(JumpIfTrue)                                                       \
    X(Call) X(Closure) X(Capture) X(Close) X(Return) X(ReturnNil)                              \
    X(Print) X(Array) X(ArrayAppend) X(IndexGet) X(IndexSet)                                   \
    X(LtJumpIfFalse) X(LeJumpIfFalse) X(AddJump) X(ModAdd) X(DivAdd) X(IndexSetAdd)

#if RUNG_COMPUTED_GOTO

#define RUNG_LABEL_ADDRESS_(op) &&rung_op_##op,
#define RUNG_DISPATCH()                                                                        \
    static const void* const rung_dispatch_table[] = {RUNG_OP_LIST(RUNG_LABEL_ADDRESS_)};      \
    RUNG_NEXT();
#define RUNG_CASE(op) rung_op_##op:
#define RUNG_NEXT() goto* rung_dispatch_table[static_cast<std::uint8_t>(RUNG_FETCH())]
#define RUNG_END_DISPATCH() __builtin_unreachable();

#else

#define RUNG_DISPATCH()  \
    for (;;) {           \
        switch (RUNG_FETCH()) {
#define RUNG_CASE(op) case RUNG_OP_ENUM::op:
#define RUNG_NEXT() continue
#define RUNG_END_DISPATCH() \
        }                   \
    }

#endif
