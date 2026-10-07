#pragma once

// Instruction dispatch for the bytecode VMs, behind macros so ladder rung 3a (computed goto) can
// swap the implementation without touching a VM's loop body (notes D4). The loop body uses
// only these four macros and never writes `switch` or `case` itself.
//
//   RUNG_DISPATCH(fetch)   starts the loop; `fetch` is an expression that reads the next opcode
//   RUNG_CASE(op)          the code of one opcode (the argument is an OpCode enumerator name)
//   RUNG_NEXT()            finish this instruction and dispatch the next one
//   RUNG_END_DISPATCH()    closes the loop opened by RUNG_DISPATCH
//
// This is the `switch` implementation: one shared indirect jump at the top of the loop. Computed
// goto replaces it with a jump at the end of every instruction, which gives the CPU's branch
// predictor one history per opcode instead of one for all of them.
#define RUNG_DISPATCH(fetch) \
    for (;;) {               \
        switch (fetch) {
#define RUNG_CASE(op) case OpCode::op:
#define RUNG_NEXT() continue
#define RUNG_END_DISPATCH() \
        }                   \
    }
