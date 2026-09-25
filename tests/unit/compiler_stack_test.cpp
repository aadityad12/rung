#include <doctest.h>

#include <string>
#include <string_view>

#include "bytecode/chunk.h"
#include "compiler_stack.h"
#include "disassembler.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/function.h"
#include "runtime/heap.h"
#include "runtime/ops.h"

namespace {

// Parses, resolves, compiles and disassembles `source`. A failure at any stage is returned as
// its formatted error, so a test that expected bytecode shows why it did not get any.
std::string dis(const std::string& source, bool gc_stress = false) {
    rung::ParseResult parsed = rung::parse(source);
    if (!parsed.ok()) return "PARSE ERROR: " + rung::format_error(*parsed.error);
    if (auto error = rung::resolve(*parsed.program)) return rung::format_error(*error);
    rung::Heap heap;
    heap.set_stress(gc_stress);
    rung::StackCompileResult compiled = rung::compile_stack(*parsed.program, heap);
    if (!compiled.ok()) return rung::format_error(*compiled.error);
    rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
    return rung::disassemble(*compiled.function);
}

}  // namespace

TEST_CASE("stack compiler: arithmetic and constants") {
    CHECK(dis("print (1 + 2) * -3;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 CONST          1 (2)\n"
          "0008    1 ADD\n"
          "0009    1 CONST          2 (3)\n"
          "0013    1 NEG\n"
          "0014    1 MUL\n"
          "0015    1 PRINT\n"
          "0016    1 NIL\n"
          "0017    1 RETURN\n");
    // Every binary operator and the literals that need no constant.
    CHECK(dis("print 7 - 1 / 2 % 3; print nil; print true; print false; print !nil;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (7)\n"
          "0004    1 CONST          1 (1)\n"
          "0008    1 CONST          2 (2)\n"
          "0012    1 DIV\n"
          "0013    1 CONST          3 (3)\n"
          "0017    1 MOD\n"
          "0018    1 SUB\n"
          "0019    1 PRINT\n"
          "0020    1 NIL\n"
          "0021    1 PRINT\n"
          "0022    1 TRUE\n"
          "0023    1 PRINT\n"
          "0024    1 FALSE\n"
          "0025    1 PRINT\n"
          "0026    1 NIL\n"
          "0027    1 NOT\n"
          "0028    1 PRINT\n"
          "0029    1 NIL\n"
          "0030    1 RETURN\n");
}

TEST_CASE("stack compiler: comparisons are single opcodes") {
    // `<=`, `>=` and `!=` are not `!(>)` etc. (NaN), so each has its own opcode.
    CHECK(dis("1 == 2; 1 != 2; 1 < 2; 1 <= 2; 1 > 2; 1 >= 2;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 CONST          1 (2)\n"
          "0008    1 EQ\n"
          "0009    1 POP\n"
          "0010    1 CONST          2 (1)\n"
          "0014    1 CONST          3 (2)\n"
          "0018    1 NE\n"
          "0019    1 POP\n"
          "0020    1 CONST          4 (1)\n"
          "0024    1 CONST          5 (2)\n"
          "0028    1 LT\n"
          "0029    1 POP\n"
          "0030    1 CONST          6 (1)\n"
          "0034    1 CONST          7 (2)\n"
          "0038    1 LE\n"
          "0039    1 POP\n"
          "0040    1 CONST          8 (1)\n"
          "0044    1 CONST          9 (2)\n"
          "0048    1 GT\n"
          "0049    1 POP\n"
          "0050    1 CONST          10 (1)\n"
          "0054    1 CONST          11 (2)\n"
          "0058    1 GE\n"
          "0059    1 POP\n"
          "0060    1 NIL\n"
          "0061    1 RETURN\n");
}

TEST_CASE("stack compiler: constants are shown by value") {
    CHECK(dis("print 1.5; print 3.0; print \"hi\\n\"; print -2147483648; print 0.1;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1.5)\n"
          "0004    1 PRINT\n"
          "0005    1 CONST          1 (3.0)\n"
          "0009    1 PRINT\n"
          "0010    1 CONST          2 (\"hi\\n\")\n"
          "0014    1 PRINT\n"
          "0015    1 CONST          3 (-2147483648)\n"
          "0019    1 PRINT\n"
          "0020    1 CONST          4 (0.1)\n"
          "0024    1 PRINT\n"
          "0025    1 NIL\n"
          "0026    1 RETURN\n");
}

TEST_CASE("stack compiler: globals are looked up by name, and names share one constant") {
    CHECK(dis("let a = 1; let b; a = a + 1; print b;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"a\")\n"
          "0008    1 NIL\n"
          "0009    1 DEFINE_GLOBAL  2 (\"b\")\n"
          "0013    1 GET_GLOBAL     1 (\"a\")\n"
          "0017    1 CONST          3 (1)\n"
          "0021    1 ADD\n"
          "0022    1 SET_GLOBAL     1 (\"a\")\n"
          "0026    1 POP\n"
          "0027    1 GET_GLOBAL     2 (\"b\")\n"
          "0031    1 PRINT\n"
          "0032    1 NIL\n"
          "0033    1 RETURN\n");
    // A string literal equal to a name reuses the same interned string, hence the same index.
    CHECK(dis("let a = \"a\";") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (\"a\")\n"
          "0004    1 DEFINE_GLOBAL  0 (\"a\")\n"
          "0008    1 NIL\n"
          "0009    1 RETURN\n");
}

TEST_CASE("stack compiler: locals use stack slots, slot 0 is the callee") {
    // In a block, `let` needs no instruction of its own: the initializer's value stays on the
    // stack and that stack position becomes the variable. Leaving the block pops them.
    CHECK(dis("{ let a = 1; let b = 2; a = b; print a + b; }") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 CONST          1 (2)\n"
          "0008    1 GET_LOCAL      2\n"
          "0010    1 SET_LOCAL      1\n"
          "0012    1 POP\n"
          "0013    1 GET_LOCAL      1\n"
          "0015    1 GET_LOCAL      2\n"
          "0017    1 ADD\n"
          "0018    1 PRINT\n"
          "0019    1 POP\n"
          "0020    1 POP\n"
          "0021    1 NIL\n"
          "0022    1 RETURN\n");
    // Parameters are the first slots, and a local shadows a parameter with a new slot.
    CHECK(dis("fn f(x, y) { let x2 = x; { let x = y; print x; } return x; }") ==
          "== <script> ==\n"
          "0000    1 CLOSURE        0 (<fn f>)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"f\")\n"
          "0008    1 NIL\n"
          "0009    1 RETURN\n"
          "\n"
          "== f (arity 2, upvalues 0) ==\n"
          "0000    1 GET_LOCAL      1\n"
          "0002    1 GET_LOCAL      2\n"
          "0004    1 GET_LOCAL      4\n"
          "0006    1 PRINT\n"
          "0007    1 POP\n"
          "0008    1 GET_LOCAL      1\n"
          "0010    1 RETURN\n"
          "0011    1 NIL\n"
          "0012    1 RETURN\n");
}

TEST_CASE("stack compiler: the D11 example keeps the closure bound to the global") {
    // show() was compiled before the block-local `a` existed, so it reads the global by name.
    // A tree-walker doing dynamic lookup would print "block" the second time.
    CHECK(dis("let a = \"global\";\n"
              "{ fn show() { print a; } show(); let a = \"block\"; show(); }") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (\"global\")\n"
          "0004    1 DEFINE_GLOBAL  1 (\"a\")\n"
          "0008    2 CLOSURE        2 (<fn show>)\n"
          "0012    2 GET_LOCAL      1\n"
          "0014    2 CALL           0\n"
          "0016    2 POP\n"
          "0017    2 CONST          3 (\"block\")\n"
          "0021    2 GET_LOCAL      1\n"
          "0023    2 CALL           0\n"
          "0025    2 POP\n"
          "0026    2 POP\n"
          "0027    2 POP\n"
          "0028    2 NIL\n"
          "0029    2 RETURN\n"
          "\n"
          "== show (arity 0, upvalues 0) ==\n"
          "0000    2 GET_GLOBAL     0 (\"a\")\n"
          "0004    2 PRINT\n"
          "0005    2 NIL\n"
          "0006    2 RETURN\n");
}

TEST_CASE("stack compiler: closures capture locals through upvalues") {
    // `n` lives in make()'s slot 1; inc() captures it (is_local = 1, index = the slot).
    // Returning closes it: make's RETURN handles that, so the body needs no CLOSE_UPVALUE.
    CHECK(dis("fn make() { let n = 0; fn inc() { n = n + 1; return n; } return inc; }") ==
          "== <script> ==\n"
          "0000    1 CLOSURE        0 (<fn make>)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"make\")\n"
          "0008    1 NIL\n"
          "0009    1 RETURN\n"
          "\n"
          "== make (arity 0, upvalues 0) ==\n"
          "0000    1 CONST          0 (0)\n"
          "0004    1 CLOSURE        1 (<fn inc>)\n"
          "0008    1   |            local 1\n"
          "0010    1 GET_LOCAL      2\n"
          "0012    1 RETURN\n"
          "0013    1 NIL\n"
          "0014    1 RETURN\n"
          "\n"
          "== inc (arity 0, upvalues 1) ==\n"
          "0000    1 GET_UPVALUE    0\n"
          "0002    1 CONST          0 (1)\n"
          "0006    1 ADD\n"
          "0007    1 SET_UPVALUE    0\n"
          "0009    1 POP\n"
          "0010    1 GET_UPVALUE    0\n"
          "0012    1 RETURN\n"
          "0013    1 NIL\n"
          "0014    1 RETURN\n");
}

TEST_CASE("stack compiler: a captured variable is closed when its scope ends") {
    // `c` is captured, so leaving the block emits CLOSE_UPVALUE (copy off the stack, then pop)
    // where an ordinary local gets POP. `cap` itself is not captured, so it gets a plain POP,
    // and locals leave in reverse order.
    CHECK(dis("{ let c = 3; fn cap() { return c; } }") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (3)\n"
          "0004    1 CLOSURE        1 (<fn cap>)\n"
          "0008    1   |            local 1\n"
          "0010    1 POP\n"
          "0011    1 CLOSE_UPVALUE\n"
          "0012    1 NIL\n"
          "0013    1 RETURN\n"
          "\n"
          "== cap (arity 0, upvalues 1) ==\n"
          "0000    1 GET_UPVALUE    0\n"
          "0002    1 RETURN\n"
          "0003    1 NIL\n"
          "0004    1 RETURN\n");
}

TEST_CASE("stack compiler: transitive captures pass through the middle function") {
    // `a` belongs to the script's block, `b` to outer(). inner() uses both, so middle() must
    // capture them too (is_local = 0: "copy my enclosing closure's upvalue") even though it
    // never mentions them itself.
    CHECK(dis("{ let a = 1;\n"
              "  fn outer() { let b = 2;\n"
              "    fn middle() { fn inner() { a = a + b; return b; } return inner; }\n"
              "    return middle; } }") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    2 CLOSURE        1 (<fn outer>)\n"
          "0008    2   |            local 1\n"
          "0010    1 POP\n"
          "0011    1 CLOSE_UPVALUE\n"
          "0012    4 NIL\n"
          "0013    4 RETURN\n"
          "\n"
          "== outer (arity 0, upvalues 1) ==\n"
          "0000    2 CONST          0 (2)\n"
          "0004    3 CLOSURE        1 (<fn middle>)\n"
          "0008    3   |            upvalue 0\n"
          "0010    3   |            local 1\n"
          "0012    4 GET_LOCAL      2\n"
          "0014    4 RETURN\n"
          "0015    2 NIL\n"
          "0016    2 RETURN\n"
          "\n"
          "== middle (arity 0, upvalues 2) ==\n"
          "0000    3 CLOSURE        0 (<fn inner>)\n"
          "0004    3   |            upvalue 0\n"
          "0006    3   |            upvalue 1\n"
          "0008    3 GET_LOCAL      1\n"
          "0010    3 RETURN\n"
          "0011    3 NIL\n"
          "0012    3 RETURN\n"
          "\n"
          "== inner (arity 0, upvalues 2) ==\n"
          "0000    3 GET_UPVALUE    0\n"
          "0002    3 GET_UPVALUE    1\n"
          "0004    3 ADD\n"
          "0005    3 SET_UPVALUE    0\n"
          "0007    3 POP\n"
          "0008    3 GET_UPVALUE    1\n"
          "0010    3 RETURN\n"
          "0011    3 NIL\n"
          "0012    3 RETURN\n");
}

TEST_CASE("stack compiler: a variable captured twice is one upvalue") {
    CHECK(dis("fn f() { let x = 1; fn g() { x = x + x; } }") ==
          "== <script> ==\n"
          "0000    1 CLOSURE        0 (<fn f>)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"f\")\n"
          "0008    1 NIL\n"
          "0009    1 RETURN\n"
          "\n"
          "== f (arity 0, upvalues 0) ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 CLOSURE        1 (<fn g>)\n"
          "0008    1   |            local 1\n"
          "0010    1 NIL\n"
          "0011    1 RETURN\n"
          "\n"
          "== g (arity 0, upvalues 1) ==\n"
          "0000    1 GET_UPVALUE    0\n"
          "0002    1 GET_UPVALUE    0\n"
          "0004    1 ADD\n"
          "0005    1 SET_UPVALUE    0\n"
          "0007    1 POP\n"
          "0008    1 NIL\n"
          "0009    1 RETURN\n");
}

TEST_CASE("stack compiler: a local function can call itself through an upvalue") {
    // The function's name is declared before its body is compiled, so the body finds it as a
    // local of the enclosing function: fact captures its own slot.
    CHECK(dis("{ fn fact(n) { if (n < 2) return 1; return n * fact(n - 1); } }") ==
          "== <script> ==\n"
          "0000    1 CLOSURE        0 (<fn fact>)\n"
          "0004    1   |            local 1\n"
          "0006    1 CLOSE_UPVALUE\n"
          "0007    1 NIL\n"
          "0008    1 RETURN\n"
          "\n"
          "== fact (arity 1, upvalues 1) ==\n"
          "0000    1 GET_LOCAL      1\n"
          "0002    1 CONST          0 (2)\n"
          "0006    1 LT\n"
          "0007    1 JUMP_IF_FALSE  -> 0021\n"
          "0011    1 POP\n"
          "0012    1 CONST          1 (1)\n"
          "0016    1 RETURN\n"
          "0017    1 JUMP           -> 0022\n"
          "0021    1 POP\n"
          "0022    1 GET_LOCAL      1\n"
          "0024    1 GET_UPVALUE    0\n"
          "0026    1 GET_LOCAL      1\n"
          "0028    1 CONST          2 (1)\n"
          "0032    1 SUB\n"
          "0033    1 CALL           1\n"
          "0035    1 MUL\n"
          "0036    1 RETURN\n"
          "0037    1 NIL\n"
          "0038    1 RETURN\n");
}

TEST_CASE("stack compiler: while loops jump back with LOOP and out with JUMP_IF_FALSE") {
    CHECK(dis("let i = 0; while (i < 3) i = i + 1;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (0)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"i\")\n"
          "0008    1 GET_GLOBAL     1 (\"i\")\n"
          "0012    1 CONST          2 (3)\n"
          "0016    1 LT\n"
          "0017    1 JUMP_IF_FALSE  -> 0040\n"
          "0021    1 POP\n"
          "0022    1 GET_GLOBAL     1 (\"i\")\n"
          "0026    1 CONST          3 (1)\n"
          "0030    1 ADD\n"
          "0031    1 SET_GLOBAL     1 (\"i\")\n"
          "0035    1 POP\n"
          "0036    1 LOOP           -> 0008\n"
          "0040    1 POP\n"
          "0041    1 NIL\n"
          "0042    1 RETURN\n");
}

TEST_CASE("stack compiler: for is a while whose variable lives in an enclosing block") {
    CHECK(dis("for (let i = 0; i < 2; i = i + 1) print i;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (0)\n"
          "0004    1 GET_LOCAL      1\n"
          "0006    1 CONST          1 (2)\n"
          "0010    1 LT\n"
          "0011    1 JUMP_IF_FALSE  -> 0033\n"
          "0015    1 POP\n"
          "0016    1 GET_LOCAL      1\n"
          "0018    1 PRINT\n"
          "0019    1 GET_LOCAL      1\n"
          "0021    1 CONST          2 (1)\n"
          "0025    1 ADD\n"
          "0026    1 SET_LOCAL      1\n"
          "0028    1 POP\n"
          "0029    1 LOOP           -> 0004\n"
          "0033    1 POP\n"
          "0034    1 POP\n"
          "0035    1 NIL\n"
          "0036    1 RETURN\n");
}

TEST_CASE("stack compiler: if / else patch forward jumps") {
    // Both branches start with a POP of the condition: JUMP_IF_FALSE does not pop.
    CHECK(dis("if (true) print 1; else print 2;") ==
          "== <script> ==\n"
          "0000    1 TRUE\n"
          "0001    1 JUMP_IF_FALSE  -> 0015\n"
          "0005    1 POP\n"
          "0006    1 CONST          0 (1)\n"
          "0010    1 PRINT\n"
          "0011    1 JUMP           -> 0021\n"
          "0015    1 POP\n"
          "0016    1 CONST          1 (2)\n"
          "0020    1 PRINT\n"
          "0021    1 NIL\n"
          "0022    1 RETURN\n");
    CHECK(dis("if (nil) print 1;") ==
          "== <script> ==\n"
          "0000    1 NIL\n"
          "0001    1 JUMP_IF_FALSE  -> 0015\n"
          "0005    1 POP\n"
          "0006    1 CONST          0 (1)\n"
          "0010    1 PRINT\n"
          "0011    1 JUMP           -> 0016\n"
          "0015    1 POP\n"
          "0016    1 NIL\n"
          "0017    1 RETURN\n");
}

TEST_CASE("stack compiler: and / or short-circuit and keep the deciding value") {
    CHECK(dis("print 1 and 2;") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 JUMP_IF_FALSE  -> 0013\n"
          "0008    1 POP\n"
          "0009    1 CONST          1 (2)\n"
          "0013    1 PRINT\n"
          "0014    1 NIL\n"
          "0015    1 RETURN\n");
    CHECK(dis("print nil or 2;") ==
          "== <script> ==\n"
          "0000    1 NIL\n"
          "0001    1 JUMP_IF_FALSE  -> 0009\n"
          "0005    1 JUMP           -> 0014\n"
          "0009    1 POP\n"
          "0010    1 CONST          0 (2)\n"
          "0014    1 PRINT\n"
          "0015    1 NIL\n"
          "0016    1 RETURN\n");
}

TEST_CASE("stack compiler: arrays") {
    CHECK(dis("let a = [1, 2]; a[0] = a[1]; let e = [];") ==
          "== <script> ==\n"
          "0000    1 CONST          0 (1)\n"
          "0004    1 CONST          1 (2)\n"
          "0008    1 ARRAY          2\n"
          "0012    1 DEFINE_GLOBAL  2 (\"a\")\n"
          "0016    1 GET_GLOBAL     2 (\"a\")\n"
          "0020    1 CONST          3 (0)\n"
          "0024    1 GET_GLOBAL     2 (\"a\")\n"
          "0028    1 CONST          4 (1)\n"
          "0032    1 INDEX_GET\n"
          "0033    1 INDEX_SET\n"
          "0034    1 POP\n"
          "0035    1 ARRAY          0\n"
          "0039    1 DEFINE_GLOBAL  5 (\"e\")\n"
          "0043    1 NIL\n"
          "0044    1 RETURN\n");
}

TEST_CASE("stack compiler: calls and returns") {
    CHECK(dis("fn add(a, b) { return a + b; }\nprint add(1, 2);\nfn none() { return; }") ==
          "== <script> ==\n"
          "0000    1 CLOSURE        0 (<fn add>)\n"
          "0004    1 DEFINE_GLOBAL  1 (\"add\")\n"
          "0008    2 GET_GLOBAL     1 (\"add\")\n"
          "0012    2 CONST          2 (1)\n"
          "0016    2 CONST          3 (2)\n"
          "0020    2 CALL           2\n"
          "0022    2 PRINT\n"
          "0023    3 CLOSURE        4 (<fn none>)\n"
          "0027    3 DEFINE_GLOBAL  5 (\"none\")\n"
          "0031    3 NIL\n"
          "0032    3 RETURN\n"
          "\n"
          "== add (arity 2, upvalues 0) ==\n"
          "0000    1 GET_LOCAL      1\n"
          "0002    1 GET_LOCAL      2\n"
          "0004    1 ADD\n"
          "0005    1 RETURN\n"
          "0006    1 NIL\n"
          "0007    1 RETURN\n"
          "\n"
          "== none (arity 0, upvalues 0) ==\n"
          "0000    3 NIL\n"
          "0001    3 RETURN\n"
          "0002    3 NIL\n"
          "0003    3 RETURN\n");
}

TEST_CASE("stack compiler: every instruction records its source line") {
    rung::ParseResult parsed = rung::parse("let a = 1;\n\nprint a\n  + 2;\n");
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::StackCompileResult compiled = rung::compile_stack(*parsed.program, heap);
    REQUIRE(compiled.ok());
    const rung::Chunk& chunk = compiled.function->chunk;
    // CONST DEFINE_GLOBAL on line 1; GET_GLOBAL on 3; CONST 2 and ADD on line 4 (the `+`).
    CHECK(chunk.line_at(0) == 1);
    CHECK(chunk.line_at(7) == 1);
    CHECK(chunk.line_at(8) == 3);
    CHECK(chunk.line_at(12) == 4);
    CHECK(chunk.line_at(16) == 4);
    CHECK(chunk.line_at(17) == 3);  // PRINT: the `print` keyword's line
    // Run-length encoded: one entry per change of line, not one per byte.
    CHECK(chunk.lines.size() == 5);
}

TEST_CASE("stack compiler: exactly 255 locals in one function") {
    // Slot 0 is the callee, so the locals occupy slots 1..255: the last one only just fits in
    // a one-byte operand. (The resolver rejects a 256th, notes D11.)
    std::string source = "fn f() { ";
    for (int i = 0; i < 255; ++i) {
        source += "let v" + std::to_string(i) + " = " + std::to_string(i) + "; ";
    }
    source += "print v0; print v254; v254 = v0; }";
    std::string text = dis(source);
    REQUIRE(text.find("error") == std::string::npos);
    CHECK(text.find("GET_LOCAL      1\n") != std::string::npos);
    CHECK(text.find("GET_LOCAL      255\n") != std::string::npos);
    CHECK(text.find("SET_LOCAL      255\n") != std::string::npos);
    CHECK(text.find("GET_LOCAL      256\n") == std::string::npos);
    // The same, in a nested block of the script (no function around it).
    std::string block = "{ ";
    for (int i = 0; i < 255; ++i) block += "let v" + std::to_string(i) + "; ";
    block += "print v254; }";
    text = dis(block);
    REQUIRE(text.find("error") == std::string::npos);
    CHECK(text.find("GET_LOCAL      255\n") != std::string::npos);
}

TEST_CASE("stack compiler: 255 parameters and 255 arguments") {
    std::string params, args;
    for (int i = 0; i < 255; ++i) {
        params += (i ? ", p" : "p") + std::to_string(i);
        args += i ? ", nil" : "nil";
    }
    std::string text = dis("fn f(" + params + ") { return p254; } f(" + args + ");");
    REQUIRE(text.find("error") == std::string::npos);
    CHECK(text.find("== f (arity 255, upvalues 0) ==") != std::string::npos);
    CHECK(text.find("GET_LOCAL      255\n") != std::string::npos);  // p254
    CHECK(text.find("CALL           255\n") != std::string::npos);
}

TEST_CASE("stack compiler: 255 captured variables in one function") {
    // c() captures 127 variables of a() and 128 of b(). The 255th upvalue has index 254, the
    // last byte-sized operand. a's parameters reach c through b's upvalues (is_local = 0);
    // b's own parameters are captured directly (is_local = 1).
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
    REQUIRE(text.find("error") == std::string::npos);
    CHECK(text.find("== b (arity 128, upvalues 127) ==") != std::string::npos);
    CHECK(text.find("== c (arity 0, upvalues 255) ==") != std::string::npos);
    CHECK(text.find("GET_UPVALUE    254\n") != std::string::npos);
    CHECK(text.find("GET_UPVALUE    255\n") == std::string::npos);
    CHECK(text.find("  |            upvalue 126\n") != std::string::npos);  // p126 via b
    CHECK(text.find("  |            local 128\n") != std::string::npos);    // q127 is b's slot 128
}

TEST_CASE("stack compiler: jumps longer than 65,535 bytes need the 24-bit operand") {
    // Each `print 1;` is CONST (4 bytes) + PRINT (1 byte): 14,000 of them are 70,000 bytes.
    std::string body;
    for (int i = 0; i < 14000; ++i) body += "print 1; ";
    std::string text = dis("let x = 0; while (x < 1) { " + body + "}");
    // `let x = 0;` is 8 bytes and the condition 9 more, so JUMP_IF_FALSE sits at 17, the body
    // starts at 22 and ends at 70,022, LOOP is 4 bytes, and the exit POP is at 70,026.
    CHECK(text.find("0017    1 JUMP_IF_FALSE  -> 70026\n") != std::string::npos);
    CHECK(text.find("70022    1 LOOP           -> 0008\n") != std::string::npos);
    CHECK(text.find("70026    1 POP\n") != std::string::npos);

    // A forward JUMP over the same amount of code: `if` with an else branch.
    text = dis("if (true) { " + body + "} else print 2;");
    CHECK(text.find("0001    1 JUMP_IF_FALSE  -> 70010\n") != std::string::npos);
    CHECK(text.find("70006    1 JUMP           -> 70016\n") != std::string::npos);
}

TEST_CASE("stack compiler: disassembly is identical under GC stress") {
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
    REQUIRE(plain.find("== inc (arity 0, upvalues 2) ==") != std::string::npos);
    CHECK(dis(source, true) == plain);
}

TEST_CASE("stack compiler: the compiled function survives collections once rooted") {
    rung::ParseResult parsed = rung::parse("fn f() { return \"kept\"; } let g = [1];");
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(rung::resolve(*parsed.program).has_value());
    rung::Heap heap;
    rung::StackCompileResult compiled = rung::compile_stack(*parsed.program, heap);
    REQUIRE(compiled.ok());
    std::string before = rung::disassemble(*compiled.function);
    {
        rung::Heap::TempRoot root(heap, rung::make_obj(compiled.function));
        heap.collect();
        heap.collect();
        CHECK(rung::disassemble(*compiled.function) == before);
        // script, f, and the strings "f", "g", "kept" are all reachable through the constants.
        CHECK(heap.stats().live_objects == 5);
    }
    heap.collect();
    CHECK(heap.stats().live_objects == 0);  // the marker was removed and nothing else holds it
}

TEST_CASE("function objects: closures and upvalues keep their targets alive") {
    rung::Heap heap;
    rung::ObjFunction* fn = heap.allocate<rung::ObjFunction>(nullptr);
    rung::Heap::TempRoot keep_fn(heap, rung::make_obj(fn));
    fn->upvalue_count = 2;
    fn->chunk.add_constant(rung::make_obj(heap.intern(std::string_view("in the constant pool"))));

    rung::Value slot = rung::make_int(7);
    {
        rung::ObjClosure* closure = heap.allocate<rung::ObjClosure>(fn);
        rung::Heap::TempRoot keep_closure(heap, rung::make_obj(closure));
        REQUIRE(closure->upvalues.size() == 2);
        CHECK(closure->upvalues[0] == nullptr);  // null until the VM fills them in

        // One upvalue still open (points at a stack slot), one closed and holding a string.
        rung::ObjUpvalue* open = heap.allocate<rung::ObjUpvalue>(&slot);
        closure->upvalues[0] = open;
        rung::ObjString* held = heap.intern(std::string_view("held only by a closed upvalue"));
        rung::Heap::TempRoot keep_held(heap, rung::make_obj(held));
        rung::ObjUpvalue* closed = heap.allocate<rung::ObjUpvalue>(&slot);
        closed->closed = rung::make_obj(held);
        closed->location = &closed->closed;
        closure->upvalues[1] = closed;

        // Rooting the closure alone keeps its function, both upvalues and the closed value.
        heap.collect();
        CHECK(heap.stats().live_objects == 6);  // fn, its constant, closure, 2 upvalues, held
        CHECK(rung::as_string(closed->closed)->chars == "held only by a closed upvalue");
        CHECK(rung::as_int(*open->location) == 7);
    }
    // Nothing reaches the closure or its upvalues now, but `fn` and its constant stay.
    heap.collect();
    CHECK(heap.stats().live_objects == 2);
}

TEST_CASE("function objects: functions and closures print as <fn NAME>") {
    rung::Heap heap;
    rung::ObjString* name = heap.intern(std::string_view("add"));
    rung::Heap::TempRoot keep_name(heap, rung::make_obj(name));
    rung::ObjFunction* fn = heap.allocate<rung::ObjFunction>(name);
    rung::Heap::TempRoot keep_fn(heap, rung::make_obj(fn));
    rung::ObjClosure* closure = heap.allocate<rung::ObjClosure>(fn);
    rung::Heap::TempRoot keep_closure(heap, rung::make_obj(closure));
    rung::ObjFunction* script = heap.allocate<rung::ObjFunction>(nullptr);

    std::string out;
    rung::print_value(rung::make_obj(fn), out);
    CHECK(out == "<fn add>");
    out.clear();
    rung::print_value(rung::make_obj(closure), out);
    CHECK(out == "<fn add>");
    out.clear();
    rung::print_value(rung::make_obj(script), out);
    CHECK(out == "<script>");
}
