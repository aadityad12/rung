#include <doctest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

#include "bytecode/chunk.h"
#include "bytecode/register_code.h"
#include "compiler_reg.h"
#include "compiler_stack.h"
#include "disassembler.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/function.h"
#include "runtime/heap.h"
#include "runtime/ops.h"

namespace {

// Parses, resolves, compiles and disassembles `source` to register bytecode. A failure at any
// stage is returned as its formatted error, so a test that expected bytecode shows why it did
// not get any.
std::string dis(const std::string& source, bool gc_stress = false) {
    rung::ParseResult parsed = rung::parse(source);
    if (!parsed.ok()) return "PARSE ERROR: " + rung::format_error(*parsed.error);
    if (auto error = rung::resolve(*parsed.program)) return rung::format_error(*error);
    rung::Heap heap;
    heap.set_stress(gc_stress);
    rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
    if (!compiled.ok()) return rung::format_error(*compiled.error);
    rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
    return rung::disassemble_register(*compiled.function);
}

// Number of instructions in the function called `name` (or the script for an empty name),
// compiled by the stack compiler / the register compiler. Both walk the constant pools for the
// function, since nested functions live there.
template <class Compile, class Visit>
std::size_t count_in(const std::string& source, std::string_view name, Compile compile,
                     Visit visit) {
    rung::ParseResult parsed = rung::parse(source);
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::ObjFunction* script = compile(*parsed.program, heap);
    REQUIRE(script != nullptr);
    rung::Heap::TempRoot root(heap, rung::make_obj(script));
    return visit(*script, name);
}

std::size_t stack_count(const std::string& source, std::string_view name = "") {
    auto compile = [](const rung::Program& p, rung::Heap& h) {
        return rung::compile_stack(p, h).function;
    };
    auto visit = [](const rung::ObjFunction& script, std::string_view fn_name) {
        std::function<const rung::ObjFunction*(const rung::ObjFunction&)> find =
            [&](const rung::ObjFunction& f) -> const rung::ObjFunction* {
            if ((f.name == nullptr ? std::string_view() : std::string_view(f.name->chars)) ==
                fn_name) {
                return &f;
            }
            for (rung::Value v : f.chunk.constants) {
                if (rung::is_function(v)) {
                    if (auto* r = find(*rung::as_function(v))) return r;
                }
            }
            return nullptr;
        };
        const rung::ObjFunction* fn = find(script);
        REQUIRE(fn != nullptr);
        std::size_t n = 0;
        std::string scratch;
        for (std::size_t off = 0; off < fn->chunk.code.size(); ++n) {
            off = rung::disassemble_instruction(fn->chunk, off, scratch);
        }
        return n;
    };
    return count_in(source, name, compile, visit);
}

std::size_t register_count(const std::string& source, std::string_view name = "") {
    auto compile = [](const rung::Program& p, rung::Heap& h) {
        return rung::compile_register(p, h).function;
    };
    auto visit = [](const rung::ObjFunction& script, std::string_view fn_name) {
        std::function<const rung::ObjFunction*(const rung::ObjFunction&)> find =
            [&](const rung::ObjFunction& f) -> const rung::ObjFunction* {
            if ((f.name == nullptr ? std::string_view() : std::string_view(f.name->chars)) ==
                fn_name) {
                return &f;
            }
            for (rung::Value v : f.reg.constants) {
                if (rung::is_function(v)) {
                    if (auto* r = find(*rung::as_function(v))) return r;
                }
            }
            return nullptr;
        };
        const rung::ObjFunction* fn = find(script);
        REQUIRE(fn != nullptr);
        return fn->reg.code.size();
    };
    return count_in(source, name, compile, visit);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("register format: fields round-trip through an instruction word") {
    using namespace rung;
    Instruction i = make_abc(RegOp::Sub, 65535, 1, 65534, kFlagBConst | kFlagCConst);
    CHECK(insn_op(i) == RegOp::Sub);
    CHECK(insn_a(i) == 65535);
    CHECK(insn_b(i) == 1);
    CHECK(insn_c(i) == 65534);
    CHECK(insn_flags(i) == 3);
    CHECK(sizeof(Instruction) == 8);

    // Bx is B and C read together, B being the low half.
    Instruction k = make_abx(RegOp::LoadK, 7, 0xFFFFFFFFu);
    CHECK(insn_a(k) == 7);
    CHECK(insn_bx(k) == 0xFFFFFFFFu);
    CHECK(insn_b(k) == 0xFFFF);
    CHECK(insn_c(k) == 0xFFFF);
    CHECK(insn_flags(k) == 0);
    Instruction split = make_abx(RegOp::LoadK, 0, 0x00030002u);
    CHECK(insn_b(split) == 2);
    CHECK(insn_c(split) == 3);

    // Jump offsets are signed and use all 32 bits.
    for (std::int32_t offset : {0, 1, -1, 70000, -70000, std::numeric_limits<std::int32_t>::max(),
                                std::numeric_limits<std::int32_t>::min()}) {
        Instruction j = make_asbx(RegOp::JumpIfFalse, 5, offset);
        CHECK(insn_op(j) == RegOp::JumpIfFalse);
        CHECK(insn_a(j) == 5);
        CHECK(insn_sbx(j) == offset);
    }
}

TEST_CASE("register compiler: arithmetic works on operands in place") {
    // Constants are operands (`k`), literals never need a LOADK first, and the result of a
    // sub-expression lives in a temporary above the operands.
    CHECK(dis("print (1 + 2) * -3;") ==
          "== <script> (frame 3) ==\n"
          "0000    1 ADD            r1 k0(1) k1(2)\n"
          "0001    1 NEG            r2 k2(3)\n"
          "0002    1 MUL            r0 r1 r2\n"
          "0003    1 PRINT          r0\n"
          "0004    1 RETURN_NIL\n");
    // Every binary operator, and the literals (nil, true, false are constants too).
    CHECK(dis("print 7 - 1 / 2 % 3; print nil; print true; print false; print !nil;"
              " print 1.5; print \"s\"; print 2 == 2;") ==
          "== <script> (frame 3) ==\n"
          "0000    1 DIV            r2 k1(1) k2(2)\n"
          "0001    1 MOD            r1 r2 k3(3)\n"
          "0002    1 SUB            r0 k0(7) r1\n"
          "0003    1 PRINT          r0\n"
          "0004    1 PRINT          k4(nil)\n"
          "0005    1 PRINT          k5(true)\n"
          "0006    1 PRINT          k6(false)\n"
          "0007    1 NOT            r0 k4(nil)\n"
          "0008    1 PRINT          r0\n"
          "0009    1 PRINT          k7(1.5)\n"
          "0010    1 PRINT          k8(\"s\")\n"
          "0011    1 EQ             r0 k2(2) k2(2)\n"
          "0012    1 PRINT          r0\n"
          "0013    1 RETURN_NIL\n");
    // Comparisons are single instructions, like the stack compiler's (NaN: `a <= b` is not
    // `!(a > b)`), and produce a bool in a register.
    CHECK(dis("{ let a = 1; let b = 2; let t = a == b; t = a != b; t = a < b; t = a <= b;"
              " t = a > b; t = a >= b; }") ==
          "== <script> (frame 3) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 EQ             r2 r0 r1\n"
          "0003    1 NE             r2 r0 r1\n"
          "0004    1 LT             r2 r0 r1\n"
          "0005    1 LE             r2 r0 r1\n"
          "0006    1 GT             r2 r0 r1\n"
          "0007    1 GE             r2 r0 r1\n"
          "0008    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: constants are shown by value and shared") {
    CHECK(dis("print 1.5; print 3.0; print \"hi\\n\"; print -2147483648; print 0.1;"
              " print 1.5; print 0.0; print -0.0;") ==
          "== <script> (frame 1) ==\n"
          "0000    1 PRINT          k0(1.5)\n"
          "0001    1 PRINT          k1(3.0)\n"
          "0002    1 PRINT          k2(\"hi\\n\")\n"
          "0003    1 PRINT          k3(-2147483648)\n"
          "0004    1 PRINT          k4(0.1)\n"
          "0005    1 PRINT          k0(1.5)\n"
          "0006    1 PRINT          k5(0.0)\n"
          "0007    1 NEG            r0 k5(0.0)\n"
          "0008    1 PRINT          r0\n"
          "0009    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: an instruction is dropped for every push and pop") {
    // `a = b + c * d`: the stack compiler pushes each operand, computes, stores, pops;
    // the register compiler names the slots and computes straight into a's register.
    const std::string body = "fn f(b, c, d) { let a = 0; STMT return a; }";
    auto with = [&](const std::string& stmt) {
        std::string s = body;
        s.replace(s.find("STMT"), 4, stmt);
        return s;
    };
    std::size_t stack_with = stack_count(with("a = b + c * d;"), "f");
    std::size_t stack_without = stack_count(with(""), "f");
    std::size_t reg_with = register_count(with("a = b + c * d;"), "f");
    std::size_t reg_without = register_count(with(""), "f");
    CHECK(stack_with - stack_without == 7);  // GET b, GET c, GET d, MUL, ADD, SET a, POP
    CHECK(reg_with - reg_without == 2);      // MUL t, c, d; ADD a, b, t

    CHECK(dis(with("a = b + c * d;")) ==
          "== <script> (frame 1) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn f>)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"f\")\n"
          "0002    1 RETURN_NIL\n"
          "\n"
          "== f (arity 3, upvalues 0, frame 5) ==\n"
          "0000    1 LOADK          r3 k0(0)\n"
          "0001    1 MUL            r4 r1 r2\n"
          "0002    1 ADD            r3 r0 r4\n"
          "0003    1 RETURN         r3\n"
          "0004    1 RETURN_NIL\n");

    // Whole snippets: fewer instructions in every case.
    struct Case {
        const char* source;
        std::size_t stack;
        std::size_t reg;
    };
    for (const Case& c : {
             Case{"{ let a = 0; let b = 1; let c = 2; let d = 3; a = b + c * d; }", 17, 7},
             Case{"let a = 1; let b = 2; a = a + b * 3; print a;", 15, 12},
             Case{"for (let i = 0; i < 3; i = i + 1) { print i; }", 18, 7},
             Case{"print (1 + 2) * -3;", 9, 5},
         }) {
        CAPTURE(c.source);
        CHECK(stack_count(c.source) == c.stack);
        CHECK(register_count(c.source) == c.reg);
        CHECK(register_count(c.source) < stack_count(c.source));
    }
}

TEST_CASE("register compiler: globals are looked up by name, and names share one constant") {
    CHECK(dis("let a = 1; let b; a = a + 1; print b;") ==
          "== <script> (frame 2) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"a\")\n"
          "0002    1 LOADNIL        r0\n"
          "0003    1 DEFINE_GLOBAL  r0 k2(\"b\")\n"
          "0004    1 GET_GLOBAL     r1 k1(\"a\")\n"
          "0005    1 ADD            r0 r1 k0(1)\n"
          "0006    1 SET_GLOBAL     r0 k1(\"a\")\n"
          "0007    1 GET_GLOBAL     r0 k2(\"b\")\n"
          "0008    1 PRINT          r0\n"
          "0009    1 RETURN_NIL\n");
    // A string literal equal to a name reuses the same interned string, hence the same index.
    CHECK(dis("let a = \"a\";") ==
          "== <script> (frame 1) ==\n"
          "0000    1 LOADK          r0 k0(\"a\")\n"
          "0001    1 DEFINE_GLOBAL  r0 k0(\"a\")\n"
          "0002    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: locals get fixed registers and need no code to declare or free") {
    // The initializer is computed straight into the new local's register; leaving the block
    // emits nothing (the stack compiler pops each one).
    CHECK(dis("{ let a = 1; let b = a + 2; { let c = b; print c; } print a; }") ==
          "== <script> (frame 3) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 ADD            r1 r0 k1(2)\n"
          "0002    1 MOVE           r2 r1\n"
          "0003    1 PRINT          r2\n"
          "0004    1 PRINT          r0\n"
          "0005    1 RETURN_NIL\n");
    // `let x;` is nil; a parameter is a register from the first; assigning a local computes
    // into its register with no move.
    CHECK(dis("fn f(p, q) { let x; x = p; p = q + 1; return x; }") ==
          "== <script> (frame 1) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn f>)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"f\")\n"
          "0002    1 RETURN_NIL\n"
          "\n"
          "== f (arity 2, upvalues 0, frame 3) ==\n"
          "0000    1 LOADNIL        r2\n"
          "0001    1 MOVE           r2 r0\n"
          "0002    1 ADD            r0 r1 k0(1)\n"
          "0003    1 RETURN         r2\n"
          "0004    1 RETURN_NIL\n");
    // A local shadowing a global, then the global again after the block.
    CHECK(dis("let a = 1; { let a = 2; print a; } print a;") ==
          "== <script> (frame 1) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"a\")\n"
          "0002    1 LOADK          r0 k2(2)\n"
          "0003    1 PRINT          r0\n"
          "0004    1 GET_GLOBAL     r0 k1(\"a\")\n"
          "0005    1 PRINT          r0\n"
          "0006    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: the D11 example keeps the closure bound to the global") {
    // `a` in show() is the global even though a local `a` is declared later in the block.
    std::string text = dis("let a = \"global\";"
                           " { fn show() { print a; } show(); let a = \"block\"; show(); }");
    CHECK(contains(text, "== show (arity 0, upvalues 0, frame 1) ==\n"
                         "0000    1 GET_GLOBAL     r0 k0(\"a\")\n"
                         "0001    1 PRINT          r0\n"
                         "0002    1 RETURN_NIL\n"));
    CHECK_FALSE(contains(text, "GET_UPVALUE"));
}

TEST_CASE("register compiler: calls put callee and arguments in consecutive registers") {
    CHECK(dis("fn add(a, b) { return a + b; } print add(1, 2);") ==
          "== <script> (frame 3) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn add>)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"add\")\n"
          "0002    1 GET_GLOBAL     r0 k1(\"add\")\n"
          "0003    1 LOADK          r1 k2(1)\n"
          "0004    1 LOADK          r2 k3(2)\n"
          "0005    1 CALL           r0 2\n"
          "0006    1 PRINT          r0\n"
          "0007    1 RETURN_NIL\n"
          "\n"
          "== add (arity 2, upvalues 0, frame 3) ==\n"
          "0000    1 ADD            r2 r0 r1\n"
          "0001    1 RETURN         r2\n"
          "0002    1 RETURN_NIL\n");
    // The result comes back in the callee's register. When the destination is a local that is
    // not the newest register, the callee sits in a temporary and one MOVE brings it home.
    CHECK(dis("{ let a = 1; let b = 2; a = f(a); b = f(b) + 1; let c = f(a, b); f(); }") ==
          "== <script> (frame 5) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 GET_GLOBAL     r2 k2(\"f\")\n"
          "0003    1 MOVE           r3 r0\n"
          "0004    1 CALL           r2 1\n"
          "0005    1 MOVE           r0 r2\n"
          "0006    1 GET_GLOBAL     r2 k2(\"f\")\n"
          "0007    1 MOVE           r3 r1\n"
          "0008    1 CALL           r2 1\n"
          "0009    1 ADD            r1 r2 k0(1)\n"
          "0010    1 GET_GLOBAL     r2 k2(\"f\")\n"
          "0011    1 MOVE           r3 r0\n"
          "0012    1 MOVE           r4 r1\n"
          "0013    1 CALL           r2 2\n"
          "0014    1 GET_GLOBAL     r3 k2(\"f\")\n"
          "0015    1 CALL           r3 0\n"
          "0016    1 RETURN_NIL\n");
    // Nested calls: an argument that is itself a call is built in the register it will occupy.
    CHECK(dis("print f(g(1), 2);") ==
          "== <script> (frame 3) ==\n"
          "0000    1 GET_GLOBAL     r0 k0(\"f\")\n"
          "0001    1 GET_GLOBAL     r1 k1(\"g\")\n"
          "0002    1 LOADK          r2 k2(1)\n"
          "0003    1 CALL           r1 1\n"
          "0004    1 LOADK          r2 k3(2)\n"
          "0005    1 CALL           r0 2\n"
          "0006    1 PRINT          r0\n"
          "0007    1 RETURN_NIL\n");
    // The callee can be any expression.
    CHECK(dis("print f(a)(b);") ==
          "== <script> (frame 2) ==\n"
          "0000    1 GET_GLOBAL     r0 k0(\"f\")\n"
          "0001    1 GET_GLOBAL     r1 k1(\"a\")\n"
          "0002    1 CALL           r0 1\n"
          "0003    1 GET_GLOBAL     r1 k2(\"b\")\n"
          "0004    1 CALL           r0 1\n"
          "0005    1 PRINT          r0\n"
          "0006    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: closures capture registers through upvalues") {
    CHECK(dis("fn counter() { let n = 0; fn inc() { n = n + 1; return n; } return inc; }") ==
          "== <script> (frame 1) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn counter>)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"counter\")\n"
          "0002    1 RETURN_NIL\n"
          "\n"
          "== counter (arity 0, upvalues 0, frame 2) ==\n"
          "0000    1 LOADK          r0 k0(0)\n"
          "0001    1 CLOSURE        r1 k1(<fn inc>)\n"
          "0002    1   |            local 0\n"
          "0003    1 RETURN         r1\n"
          "0004    1 RETURN_NIL\n"
          "\n"
          "== inc (arity 0, upvalues 1, frame 2) ==\n"
          "0000    1 GET_UPVALUE    r1 0\n"
          "0001    1 ADD            r0 r1 k0(1)\n"
          "0002    1 SET_UPVALUE    r0 0\n"
          "0003    1 GET_UPVALUE    r0 0\n"
          "0004    1 RETURN         r0\n"
          "0005    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: a captured variable is closed when its scope ends") {
    // `b` is captured, `a` is used by nobody: leaving the inner block closes from b's register.
    // Leaving the outer block closes `a` (captured too, by f).
    CHECK(dis("{ let a = 1; { let b = 2; fn f() { return a + b; } print f(); } print a; }") ==
          "== <script> (frame 4) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 CLOSURE        r2 k2(<fn f>)\n"
          "0003    1   |            local 0\n"
          "0004    1   |            local 1\n"
          "0005    1 MOVE           r3 r2\n"
          "0006    1 CALL           r3 0\n"
          "0007    1 PRINT          r3\n"
          "0008    1 CLOSE          r1\n"
          "0009    1 PRINT          r0\n"
          "0010    1 CLOSE          r0\n"
          "0011    1 RETURN_NIL\n"
          "\n"
          "== f (arity 0, upvalues 2, frame 3) ==\n"
          "0000    1 GET_UPVALUE    r1 0\n"
          "0001    1 GET_UPVALUE    r2 1\n"
          "0002    1 ADD            r0 r1 r2\n"
          "0003    1 RETURN         r0\n"
          "0004    1 RETURN_NIL\n");
    // A block with no captured variable gets no CLOSE.
    CHECK_FALSE(contains(dis("{ let a = 1; { let b = 2; print a + b; } }"), "CLOSE"));
    // In a loop the CLOSE runs at the end of every iteration, so each closure gets its own
    // variable (notes §2.6 makes the `for` variable the exception: it lives outside the body).
    std::string loop =
        dis("let i = 0; while (i < 2) { let x = i; fn g() { return x; } i = i + 1; }");
    CHECK(contains(loop, "0006    1 CLOSURE        r1 k3(<fn g>)\n"
                         "0007    1   |            local 0\n"));
    CHECK(contains(loop, "0011    1 CLOSE          r0\n"
                         "0012    1 JUMP           -> 0002\n"));
}

TEST_CASE("register compiler: transitive captures pass through the middle function") {
    CHECK(dis("fn outer() { let x = 1; fn mid() { fn inner() { return x; } return inner; } "
              "return mid; }") ==
          "== <script> (frame 1) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn outer>)\n"
          "0001    1 DEFINE_GLOBAL  r0 k1(\"outer\")\n"
          "0002    1 RETURN_NIL\n"
          "\n"
          "== outer (arity 0, upvalues 0, frame 2) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 CLOSURE        r1 k1(<fn mid>)\n"
          "0002    1   |            local 0\n"
          "0003    1 RETURN         r1\n"
          "0004    1 RETURN_NIL\n"
          "\n"
          "== mid (arity 0, upvalues 1, frame 1) ==\n"
          "0000    1 CLOSURE        r0 k0(<fn inner>)\n"
          "0001    1   |            upvalue 0\n"
          "0002    1 RETURN         r0\n"
          "0003    1 RETURN_NIL\n"
          "\n"
          "== inner (arity 0, upvalues 1, frame 1) ==\n"
          "0000    1 GET_UPVALUE    r0 0\n"
          "0001    1 RETURN         r0\n"
          "0002    1 RETURN_NIL\n");
    // Two variables from two different functions: `x` (local of f) and `y` (local of g)
    // reach g's inner function as upvalues 0 (from g's upvalue) and 1 (g's own register).
    std::string text = dis("{ let x = 1; fn f() { let y = 2; fn g() { x = x + y; } return g; } }");
    CHECK(contains(text, "== f (arity 0, upvalues 1, frame 2) ==\n"
                         "0000    1 LOADK          r0 k0(2)\n"
                         "0001    1 CLOSURE        r1 k1(<fn g>)\n"
                         "0002    1   |            upvalue 0\n"
                         "0003    1   |            local 0\n"));
    CHECK(contains(text, "== g (arity 0, upvalues 2, frame 3) ==\n"
                         "0000    1 GET_UPVALUE    r1 0\n"
                         "0001    1 GET_UPVALUE    r2 1\n"
                         "0002    1 ADD            r0 r1 r2\n"
                         "0003    1 SET_UPVALUE    r0 0\n"));
}

TEST_CASE("register compiler: a local function can call itself through an upvalue") {
    // The function's own register is declared before its body is compiled, so the body can
    // capture it (notes D11); the closure lands in that register.
    std::string text = dis("{ fn fact(n) { if (n < 2) return 1; return n * fact(n - 1); }"
                           " print fact(5); }");
    CHECK(contains(text, "0000    1 CLOSURE        r0 k0(<fn fact>)\n"
                         "0001    1   |            local 0\n"));
    // `n * fact(n - 1)`: the call could assign to `n` (through a closure), so n is copied to
    // r2 before it.
    CHECK(contains(text, "== fact (arity 1, upvalues 1, frame 5) ==\n"
                         "0000    1 LT             r1 r0 k0(2)\n"
                         "0001    1 JUMP_IF_FALSE  r1 -> 0003\n"
                         "0002    1 RETURN         k1(1)\n"
                         "0003    1 MOVE           r2 r0\n"
                         "0004    1 GET_UPVALUE    r3 0\n"
                         "0005    1 SUB            r4 r0 k1(1)\n"
                         "0006    1 CALL           r3 1\n"
                         "0007    1 MUL            r1 r2 r3\n"
                         "0008    1 RETURN         r1\n"
                         "0009    1 RETURN_NIL\n"));
}

TEST_CASE("register compiler: loops jump back with a negative offset") {
    // A `for` is a `while` whose variable lives in an enclosing block, so `i` is a register.
    CHECK(dis("for (let i = 0; i < 3; i = i + 1) { print i; }") ==
          "== <script> (frame 2) ==\n"
          "0000    1 LOADK          r0 k0(0)\n"
          "0001    1 LT             r1 r0 k1(3)\n"
          "0002    1 JUMP_IF_FALSE  r1 -> 0006\n"
          "0003    1 PRINT          r0\n"
          "0004    1 ADD            r0 r0 k2(1)\n"
          "0005    1 JUMP           -> 0001\n"
          "0006    1 RETURN_NIL\n");
    // A local used as the condition is tested in place.
    CHECK(dis("{ let go = true; while (go) { go = false; } }") ==
          "== <script> (frame 1) ==\n"
          "0000    1 LOADTRUE       r0\n"
          "0001    1 JUMP_IF_FALSE  r0 -> 0004\n"
          "0002    1 LOADFALSE      r0\n"
          "0003    1 JUMP           -> 0001\n"
          "0004    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: if / else patch forward jumps") {
    CHECK(dis("{ let i = 0; if (i) print 1; if (i == 0) { print 2; } else { print 3; } }") ==
          "== <script> (frame 2) ==\n"
          "0000    1 LOADK          r0 k0(0)\n"
          "0001    1 JUMP_IF_FALSE  r0 -> 0003\n"
          "0002    1 PRINT          k1(1)\n"
          "0003    1 EQ             r1 r0 k0(0)\n"
          "0004    1 JUMP_IF_FALSE  r1 -> 0007\n"
          "0005    1 PRINT          k2(2)\n"
          "0006    1 JUMP           -> 0008\n"
          "0007    1 PRINT          k3(3)\n"
          "0008    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: and / or short-circuit and keep the deciding value") {
    // The left value goes into the destination; the jump keeps it or lets the right overwrite it.
    CHECK(dis("{ let r = 1; let x = r and 2 or 3; }") ==
          "== <script> (frame 2) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 MOVE           r1 r0\n"
          "0002    1 JUMP_IF_FALSE  r1 -> 0004\n"
          "0003    1 LOADK          r1 k1(2)\n"
          "0004    1 JUMP_IF_TRUE   r1 -> 0006\n"
          "0005    1 LOADK          r1 k2(3)\n"
          "0006    1 RETURN_NIL\n");
    // Assigning to a variable that the expression reads: building the value in the variable's
    // own register would clobber `a` before the right operand reads it, so a temporary is used.
    CHECK(dis("{ let a = 1; let b = 2; a = (b + 1) and a; }") ==
          "== <script> (frame 3) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 ADD            r2 r1 k0(1)\n"
          "0003    1 JUMP_IF_FALSE  r2 -> 0005\n"
          "0004    1 MOVE           r2 r0\n"
          "0005    1 MOVE           r0 r2\n"
          "0006    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: operands that a later operand could change are copied") {
    // `a + (a = 5)` is 1 + 5 = 6 (the left is read first). Reading register a in place at the
    // ADD would give 5 + 5, so a copy is taken.
    CHECK(dis("{ let a = 1; print a + (a = 5); }") ==
          "== <script> (frame 4) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 MOVE           r2 r0\n"
          "0002    1 LOADK          r0 k1(5)\n"
          "0003    1 MOVE           r3 r0\n"
          "0004    1 ADD            r1 r2 r3\n"
          "0005    1 PRINT          r1\n"
          "0006    1 RETURN_NIL\n");
    // A call can assign to a captured local, so it forces the same copy.
    CHECK(dis("{ let a = 1; fn f() { a = 9; return 0; } print a + f(); }") ==
          "== <script> (frame 5) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 CLOSURE        r1 k1(<fn f>)\n"
          "0002    1   |            local 0\n"
          "0003    1 MOVE           r3 r0\n"
          "0004    1 MOVE           r4 r1\n"
          "0005    1 CALL           r4 0\n"
          "0006    1 ADD            r2 r3 r4\n"
          "0007    1 PRINT          r2\n"
          "0008    1 CLOSE          r0\n"
          "0009    1 RETURN_NIL\n"
          "\n"
          "== f (arity 0, upvalues 1, frame 1) ==\n"
          "0000    1 LOADK          r0 k0(9)\n"
          "0001    1 SET_UPVALUE    r0 0\n"
          "0002    1 RETURN         k1(0)\n"
          "0003    1 RETURN_NIL\n");
    // Without a possible assignment (a global read, a literal) the register is used in place.
    CHECK(dis("{ let a = 1; let b = 2; print a + b; }") ==
          "== <script> (frame 3) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 ADD            r2 r0 r1\n"
          "0003    1 PRINT          r2\n"
          "0004    1 RETURN_NIL\n");
    CHECK(contains(dis("{ let a = 1; print a + g; }"), "ADD            r1 r0 r2\n"));
    // The same for the array and index of an index expression, and for an indexed assignment.
    CHECK(dis("{ let a = 1; let i = 0; print a[i = 1]; }") ==
          "== <script> (frame 5) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(0)\n"
          "0002    1 MOVE           r3 r0\n"
          "0003    1 LOADK          r1 k0(1)\n"
          "0004    1 MOVE           r4 r1\n"
          "0005    1 INDEX_GET      r2 r3 r4\n"
          "0006    1 PRINT          r2\n"
          "0007    1 RETURN_NIL\n");
    CHECK(dis("{ let a = 1; let b = 2; a[b = 0] = (b = 3); }") ==
          "== <script> (frame 5) ==\n"
          "0000    1 LOADK          r0 k0(1)\n"
          "0001    1 LOADK          r1 k1(2)\n"
          "0002    1 MOVE           r2 r0\n"
          "0003    1 LOADK          r1 k2(0)\n"
          "0004    1 MOVE           r3 r1\n"
          "0005    1 LOADK          r1 k3(3)\n"
          "0006    1 MOVE           r4 r1\n"
          "0007    1 INDEX_SET      r2 r3 r4\n"
          "0008    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: arrays and indexing") {
    CHECK(dis("let a = []; let b = [1, [2, 3]]; print b[1][0]; a = array(3, 0); a[1] = 5;") ==
          "== <script> (frame 5) ==\n"
          "0000    1 ARRAY          r0 r1 0\n"
          "0001    1 DEFINE_GLOBAL  r0 k0(\"a\")\n"
          "0002    1 LOADK          r1 k1(1)\n"
          "0003    1 LOADK          r3 k2(2)\n"
          "0004    1 LOADK          r4 k3(3)\n"
          "0005    1 ARRAY          r2 r3 2\n"
          "0006    1 ARRAY          r0 r1 2\n"
          "0007    1 DEFINE_GLOBAL  r0 k4(\"b\")\n"
          "0008    1 GET_GLOBAL     r2 k4(\"b\")\n"
          "0009    1 INDEX_GET      r1 r2 k1(1)\n"
          "0010    1 INDEX_GET      r0 r1 k5(0)\n"
          "0011    1 PRINT          r0\n"
          "0012    1 GET_GLOBAL     r0 k6(\"array\")\n"
          "0013    1 LOADK          r1 k3(3)\n"
          "0014    1 LOADK          r2 k5(0)\n"
          "0015    1 CALL           r0 2\n"
          "0016    1 SET_GLOBAL     r0 k0(\"a\")\n"
          "0017    1 GET_GLOBAL     r0 k0(\"a\")\n"
          "0018    1 INDEX_SET      r0 k1(1) k7(5)\n"
          "0019    1 RETURN_NIL\n");
    // The value of an assignment expression is the assigned value.
    CHECK(dis("{ let a = [0]; let v = a[0] = 7; }") ==
          "== <script> (frame 2) ==\n"
          "0000    1 LOADK          r1 k0(0)\n"
          "0001    1 ARRAY          r0 r1 1\n"
          "0002    1 INDEX_SET      r0 k0(0) k1(7)\n"
          "0003    1 LOADK          r1 k1(7)\n"
          "0004    1 RETURN_NIL\n");
    // A literal indexed directly is loaded into a register for INDEX_SET (its array operand
    // is a register), but INDEX_GET takes it as a constant.
    CHECK(dis("print \"abc\"[0]; \"abc\"[0] = 1;") ==
          "== <script> (frame 1) ==\n"
          "0000    1 INDEX_GET      r0 k0(\"abc\") k1(0)\n"
          "0001    1 PRINT          r0\n"
          "0002    1 LOADK          r0 k0(\"abc\")\n"
          "0003    1 INDEX_SET      r0 k1(0) k2(1)\n"
          "0004    1 RETURN_NIL\n");
}

TEST_CASE("register compiler: array literals longer than one batch") {
    // 120 elements are gathered as 50 + 50 + 20, one register block reused for each batch. The
    // array is built in a temporary, so a `let` initializer can read the variable it replaces.
    std::string elements;
    for (int i = 0; i < 120; ++i) elements += (i ? ", " : "") + std::to_string(i);
    std::string text = dis("{ let a = [" + elements + "]; }");
    CHECK(contains(text, "== <script> (frame 51) ==\n"));
    CHECK(contains(text, "0049    1 LOADK          r50 k49(49)\n"
                         "0050    1 ARRAY          r0 r1 50\n"));
    CHECK(contains(text, "0101    1 ARRAY_APPEND   r0 r1 50\n"));
    CHECK(contains(text, "0122    1 ARRAY_APPEND   r0 r1 20\n"));
    CHECK(contains(text, "0123    1 RETURN_NIL\n"));

    // Assigned to a live variable, the half-built array must not be in the variable's register:
    // the elements read `a`.
    text = dis("{ let a = 1; a = [" + elements + ", a]; }");
    CHECK(contains(text, "ARRAY          r1 r2 50\n"));
    CHECK(contains(text, "ARRAY_APPEND   r1 r2 21\n"));
    CHECK(contains(text, "MOVE           r0 r1\n"));
}

TEST_CASE("register compiler: every instruction records its source line") {
    rung::ParseResult parsed = rung::parse("let a = 1;\n\nprint a\n  + 2;\n");
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
    REQUIRE(compiled.ok());
    const rung::RegChunk& chunk = compiled.function->reg;
    // LOADK, DEFINE_GLOBAL on line 1; GET_GLOBAL on 3; ADD on line 4 (the `+`); PRINT on 3;
    // RETURN_NIL on line 5 (where the end of the input is).
    REQUIRE(chunk.code.size() == 6);
    CHECK(chunk.line_at(0) == 1);
    CHECK(chunk.line_at(1) == 1);
    CHECK(chunk.line_at(2) == 3);
    CHECK(chunk.line_at(3) == 4);
    CHECK(chunk.line_at(4) == 3);
    CHECK(chunk.line_at(5) == 5);
    CHECK(chunk.lines.size() == 5);  // run-length encoded: one entry per change of line
}

TEST_CASE("register compiler: frame size is the highest register used, plus one") {
    auto frame = [](const std::string& source, std::string_view name = "") {
        rung::ParseResult parsed = rung::parse(source);
        REQUIRE(parsed.ok());
        REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
        rung::Heap heap;
        rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
        REQUIRE(compiled.ok());
        rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
        if (name.empty()) return compiled.function->reg.frame_size;
        for (rung::Value v : compiled.function->reg.constants) {
            if (rung::is_function(v) && rung::as_function(v)->name->chars == name) {
                return rung::as_function(v)->reg.frame_size;
            }
        }
        FAIL("function not found: " << name);
        return -1;
    };
    CHECK(frame("") == 0);
    CHECK(frame("print 1;") == 0);            // a constant operand needs no register at all
    CHECK(frame("print 1 + 2;") == 1);        // the sum goes into one temporary
    CHECK(frame("print (1 + 2) * (3 + 4);") == 3);  // result + two sub-results
    CHECK(frame("{ let a = 1; print a; }") == 1);   // a local read in place
    CHECK(frame("{ let a = 1; let b = 2; print a + b; }") == 3);
    // Parameters count, and a call places its callee and arguments in registers.
    CHECK(frame("fn f(a, b, c) { return a; }", "f") == 3);
    CHECK(frame("f(1, 2, 3);") == 4);
    CHECK(frame("fn f(a) { return a + (a + (a + a)); }", "f") == 4);
    // Temporaries are freed when their expression ends: two statements reuse the same ones.
    CHECK(frame("print 1 + 2; print 3 + 4;") == 1);
    CHECK(frame("{ let a = 1; { let b = 2; let c = 3; } { let d = 4; } }") == 3);
    // Each function has its own frame.
    CHECK(frame("fn f() { let a = 1; let b = 2; let c = 3; } print 1;") == 1);  // f's closure
    CHECK(frame("fn f() { let a = 1; let b = 2; let c = 3; }", "f") == 3);
}

TEST_CASE("register compiler: exactly 255 locals in one function") {
    // The locals occupy registers 0..254 (there is no slot for the callee, unlike the stack
    // compiler); the resolver rejects a 256th (notes D11).
    std::string source = "fn f() { ";
    for (int i = 0; i < 255; ++i) {
        source += "let v" + std::to_string(i) + " = " + std::to_string(i) + "; ";
    }
    source += "print v0; print v254; v254 = v0; }";
    std::string text = dis(source);
    REQUIRE_FALSE(contains(text, "error"));
    CHECK(contains(text, "== f (arity 0, upvalues 0, frame 255) ==\n"));
    CHECK(contains(text, "PRINT          r0\n"));
    CHECK(contains(text, "PRINT          r254\n"));
    CHECK(contains(text, "MOVE           r254 r0\n"));
    CHECK_FALSE(contains(text, "r255"));
}

TEST_CASE("register compiler: 255 parameters and 255 arguments") {
    std::string params, args;
    for (int i = 0; i < 255; ++i) {
        params += (i ? ", p" : "p") + std::to_string(i);
        args += i ? ", nil" : "nil";
    }
    std::string text = dis("fn f(" + params + ") { return p254; } f(" + args + ");");
    REQUIRE_FALSE(contains(text, "error"));
    CHECK(contains(text, "== f (arity 255, upvalues 0, frame 255) ==\n"));
    CHECK(contains(text, "RETURN         r254\n"));
    // The callee is r0 and the arguments r1..r255.
    CHECK(contains(text, "== <script> (frame 256) ==\n"));
    CHECK(contains(text, "LOADNIL        r255\n"));
    CHECK(contains(text, "CALL           r0 255\n"));
}

TEST_CASE("register compiler: 255 captured variables in one function") {
    std::string a_params, b_params, uses;
    for (int i = 0; i < 127; ++i) {
        a_params += (i ? ", p" : "p") + std::to_string(i);
        uses += "p" + std::to_string(i) + "; ";
    }
    for (int i = 0; i < 128; ++i) {
        b_params += (i ? ", q" : "q") + std::to_string(i);
        uses += "q" + std::to_string(i) + "; ";
    }
    std::string text = dis("fn a(" + a_params + ") { fn b(" + b_params + ") { fn c() { " + uses +
                           "} } }");
    REQUIRE_FALSE(contains(text, "error"));
    CHECK(contains(text, "== b (arity 128, upvalues 127, frame 129) ==\n"));
    CHECK(contains(text, "== c (arity 0, upvalues 255, frame 1) ==\n"));
    CHECK(contains(text, "GET_UPVALUE    r0 254\n"));
    CHECK(contains(text, "  |            upvalue 126\n"));  // p126 through b
    CHECK(contains(text, "  |            local 127\n"));    // q127 is b's register 127
}

TEST_CASE("register compiler: expressions nested at the 200 limit need only a few registers") {
    // The frame grows by about one register per level of nesting, far below the 65,535 that
    // 16-bit operands allow. All four shapes are accepted at depth 200 (and rejected at 201 by
    // the parser, before this compiler runs).
    auto nested = [](const std::string& open, const std::string& close, int depth) {
        std::string out;
        for (int i = 0; i < depth; ++i) out += open;
        out += "1";
        for (int i = 0; i < depth; ++i) out += close;
        return out;
    };
    std::string text = dis("print " + nested("1 + (", ")", 200) + ";");
    CHECK(contains(text, "== <script> (frame 200) ==\n"));
    text = dis("print " + nested("-", "", 200) + ";");
    CHECK(contains(text, "== <script> (frame 200) ==\n"));
    text = dis("fn f(x) { return x; } print " + nested("f(", ")", 200) + ";");
    CHECK(contains(text, "== <script> (frame 201) ==\n"));
    text = dis("print " + nested("[", "]", 200) + ";");
    CHECK(contains(text, "== <script> (frame 201) ==\n"));
    text = dis(std::string(200, '{') + "let a = 1; print a;" + std::string(200, '}'));
    CHECK(contains(text, "== <script> (frame 1) ==\n"));
    // Something over the limit is the parser's error, not ours.
    CHECK(contains(dis("print " + nested("1 + (", ")", 201) + ";"), "nesting too deep"));

    // The worst case for registers: 200 nested calls, each with 255 arguments, the nested call
    // being the last one. About 255 registers per level, still under 16 bits.
    std::string args;
    for (int i = 0; i < 254; ++i) args += "1, ";
    std::string call;
    for (int i = 0; i < 200; ++i) call += "f(" + args;
    call += "1";
    for (int i = 0; i < 200; ++i) call += ")";
    text = dis("fn f() {} print " + call + ";");
    REQUIRE_FALSE(contains(text, "error"));
    CHECK(contains(text, "== <script> (frame 51001) ==\n"));
}

TEST_CASE("register compiler: a function with more than 65,535 constants") {
    // 70,000 distinct integers: those with pool index up to 65,535 are RK operands, the rest
    // are loaded with LOADK (whose 32-bit index reaches them all) first. Nothing is rejected.
    std::string source;
    for (int i = 1; i <= 70000; ++i) source += "print " + std::to_string(i) + ";\n";
    source += "{ let b = 1; b = b + 80000; }\nprint zz;\n";
    std::string text = dis(source);
    REQUIRE_FALSE(contains(text, "error"));
    CHECK(contains(text, "PRINT          k65535(65536)\n"));
    // The next constant no longer fits an RK field.
    CHECK(contains(text, "LOADK          r0 k65536(65537)\n"));
    CHECK(contains(text, "LOADK          r1 k70000(80000)\n"));
    CHECK(contains(text, "ADD            r0 r0 r1\n"));
    // A global's name is reached through the 32-bit Bx too.
    CHECK(contains(text, "GET_GLOBAL     r0 k70001(\"zz\")\n"));
}

TEST_CASE("register compiler: jumps longer than 65,535 instructions") {
    // One instruction per `print 1;`, so 70,000 of them are a jump of 70,000 instructions.
    std::string body;
    for (int i = 0; i < 70000; ++i) body += "print 1; ";
    std::string text = dis("let x = 0; while (x < 1) { " + body + "}");
    // 0-1 `let x = 0;`; 2-3 the condition; 4 JUMP_IF_FALSE; the body is 5..70004; 70005 JUMP.
    CHECK(contains(text, "0004    1 JUMP_IF_FALSE  r0 -> 70006\n"));
    CHECK(contains(text, "70005    1 JUMP           -> 0002\n"));
    CHECK(contains(text, "70006    1 RETURN_NIL\n"));

    // A forward JUMP over the same amount of code: `if` with an else branch.
    text = dis("if (true) { " + body + "} else print 2;");
    CHECK(contains(text, "0001    1 JUMP_IF_FALSE  r0 -> 70003\n"));
    CHECK(contains(text, "70002    1 JUMP           -> 70004\n"));
}

TEST_CASE("register compiler: disassembly is identical under GC stress") {
    // With a collection before every allocation, any function, name or literal that is not
    // rooted while it is being built is freed and the listing changes (or ASan complains).
    std::string source =
        "let g = \"global\";\n"
        "fn make(a, b) { let n = a; fn inc() { n = n + b; return n; }\n"
        "  { let s = \"inside\"; fn peek() { return s; } print peek(); }\n"
        "  return inc; }\n"
        "let f = make(1, 2);\n"
        "print f() and [1.5, \"two\", nil][0] or g;\n";
    std::string plain = dis(source, false);
    REQUIRE(contains(plain, "== inc (arity 0, upvalues 2, frame 3) ==\n"));
    CHECK(dis(source, true) == plain);
}

TEST_CASE("register compiler: the compiled function survives collections once rooted") {
    rung::ParseResult parsed = rung::parse("fn f() { return \"kept\"; } let g = [1];");
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
    REQUIRE(compiled.ok());
    std::string before = rung::disassemble_register(*compiled.function);
    {
        rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
        heap.collect();
        heap.collect();
        CHECK(rung::disassemble_register(*compiled.function) == before);
        // script, f, and the strings "f", "g", "kept" are reachable through the constants.
        CHECK(heap.stats().live_objects == 5);
    }
    heap.collect();
    CHECK(heap.stats().live_objects == 0);  // the marker was removed and nothing else holds it
}

TEST_CASE("register compiler: only the register code is filled in") {
    rung::ParseResult parsed = rung::parse("fn f() { return 1; } print f();");
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
    REQUIRE(compiled.ok());
    rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
    CHECK(compiled.function->chunk.code.empty());
    CHECK_FALSE(compiled.function->reg.code.empty());
    CHECK(compiled.function->name == nullptr);
    CHECK(compiled.function->arity == 0);
}
