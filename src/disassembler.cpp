#include "disassembler.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "runtime/ops.h"

// The row layout follows clox's debug.c (Crafting Interpreters ch. 14), except that the line
// is printed on every row and jump rows show their absolute target.

namespace rung {

namespace {

const char* op_name(OpCode op) {
    switch (op) {
        case OpCode::Const: return "CONST";
        case OpCode::Nil: return "NIL";
        case OpCode::True: return "TRUE";
        case OpCode::False: return "FALSE";
        case OpCode::Pop: return "POP";
        case OpCode::GetLocal: return "GET_LOCAL";
        case OpCode::SetLocal: return "SET_LOCAL";
        case OpCode::GetGlobal: return "GET_GLOBAL";
        case OpCode::SetGlobal: return "SET_GLOBAL";
        case OpCode::DefineGlobal: return "DEFINE_GLOBAL";
        case OpCode::GetUpvalue: return "GET_UPVALUE";
        case OpCode::SetUpvalue: return "SET_UPVALUE";
        case OpCode::Add: return "ADD";
        case OpCode::Sub: return "SUB";
        case OpCode::Mul: return "MUL";
        case OpCode::Div: return "DIV";
        case OpCode::Mod: return "MOD";
        case OpCode::Neg: return "NEG";
        case OpCode::Not: return "NOT";
        case OpCode::Eq: return "EQ";
        case OpCode::Ne: return "NE";
        case OpCode::Lt: return "LT";
        case OpCode::Le: return "LE";
        case OpCode::Gt: return "GT";
        case OpCode::Ge: return "GE";
        case OpCode::Jump: return "JUMP";
        case OpCode::JumpIfFalse: return "JUMP_IF_FALSE";
        case OpCode::Loop: return "LOOP";
        case OpCode::Call: return "CALL";
        case OpCode::Closure: return "CLOSURE";
        case OpCode::CloseUpvalue: return "CLOSE_UPVALUE";
        case OpCode::Return: return "RETURN";
        case OpCode::Print: return "PRINT";
        case OpCode::Array: return "ARRAY";
        case OpCode::IndexGet: return "INDEX_GET";
        case OpCode::IndexSet: return "INDEX_SET";
    }
    return "UNKNOWN";
}

std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {  // the escapes Rung string literals have (notes D9)
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out + "\"";
}

// A constant as the disassembler shows it: the language's own print form (runtime/ops.h), except
// that strings are quoted so `"a"` and `a` are distinguishable.
std::string constant_text(Value v) {
    if (is_string(v)) return quoted(as_string(v)->chars);
    std::string out;
    print_value(v, out);
    return out;
}

void append_format(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void append_format(std::string& out, const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    out += buf;
}

void row_start(std::string& out, const Chunk& chunk, std::size_t offset, const char* name,
               bool has_operands) {
    append_format(out, "%04zu %4d ", offset, chunk.line_at(offset));
    if (has_operands) {
        append_format(out, "%-14s", name);
    } else {
        out += name;
    }
}

}  // namespace

std::size_t disassemble_instruction(const Chunk& chunk, std::size_t offset, std::string& out) {
    const std::vector<std::uint8_t>& code = chunk.code;
    auto op = static_cast<OpCode>(code[offset]);
    const char* name = op_name(op);

    // An instruction whose operand bytes run past the end can only come from a bug in the
    // compiler; say so instead of reading out of bounds.
    auto truncated = [&](std::size_t operand_bytes) {
        if (offset + 1 + operand_bytes <= code.size()) return false;
        row_start(out, chunk, offset, name, false);
        out += " <truncated>\n";
        return true;
    };

    switch (op) {
        case OpCode::Nil:
        case OpCode::True:
        case OpCode::False:
        case OpCode::Pop:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::Div:
        case OpCode::Mod:
        case OpCode::Neg:
        case OpCode::Not:
        case OpCode::Eq:
        case OpCode::Ne:
        case OpCode::Lt:
        case OpCode::Le:
        case OpCode::Gt:
        case OpCode::Ge:
        case OpCode::CloseUpvalue:
        case OpCode::Return:
        case OpCode::Print:
        case OpCode::IndexGet:
        case OpCode::IndexSet:
            row_start(out, chunk, offset, name, false);
            out += '\n';
            return offset + 1;

        case OpCode::GetLocal:
        case OpCode::SetLocal:
        case OpCode::GetUpvalue:
        case OpCode::SetUpvalue:
        case OpCode::Call:
            if (truncated(1)) return code.size();
            row_start(out, chunk, offset, name, true);
            append_format(out, " %u\n", static_cast<unsigned>(code[offset + 1]));
            return offset + 2;

        case OpCode::Array:
            if (truncated(3)) return code.size();
            row_start(out, chunk, offset, name, true);
            append_format(out, " %u\n", static_cast<unsigned>(get_u24(&code[offset + 1])));
            return offset + 4;

        case OpCode::Const:
        case OpCode::GetGlobal:
        case OpCode::SetGlobal:
        case OpCode::DefineGlobal: {
            if (truncated(3)) return code.size();
            std::uint32_t index = get_u24(&code[offset + 1]);
            row_start(out, chunk, offset, name, true);
            append_format(out, " %u (", static_cast<unsigned>(index));
            out += index < chunk.constants.size() ? constant_text(chunk.constants[index])
                                                  : "<bad constant>";
            out += ")\n";
            return offset + 4;
        }

        case OpCode::Jump:
        case OpCode::JumpIfFalse:
        case OpCode::Loop: {
            if (truncated(3)) return code.size();
            std::size_t distance = get_u24(&code[offset + 1]);
            std::size_t next = offset + 4;
            std::size_t target = op == OpCode::Loop ? next - distance : next + distance;
            row_start(out, chunk, offset, name, true);
            append_format(out, " -> %04zu\n", target);
            return next;
        }

        case OpCode::Closure: {
            if (truncated(3)) return code.size();
            std::uint32_t index = get_u24(&code[offset + 1]);
            row_start(out, chunk, offset, name, true);
            append_format(out, " %u (", static_cast<unsigned>(index));
            bool is_fn = index < chunk.constants.size() && is_function(chunk.constants[index]);
            out += is_fn ? constant_text(chunk.constants[index]) : "<bad constant>";
            out += ")\n";
            std::size_t next = offset + 4;
            if (!is_fn) return next;
            int count = as_function(chunk.constants[index])->upvalue_count;
            for (int i = 0; i < count; ++i) {
                if (next + 2 > code.size()) {
                    out += "     <truncated>\n";
                    return code.size();
                }
                // The captured-variable pairs are operand bytes of this instruction, so they
                // are shown at their own offsets, under the CLOSURE row.
                append_format(out, "%04zu %4d %-14s %s %u\n", next, chunk.line_at(next), "  |",
                              code[next] != 0 ? "local" : "upvalue",
                              static_cast<unsigned>(code[next + 1]));
                next += 2;
            }
            return next;
        }
    }
    row_start(out, chunk, offset, "UNKNOWN", true);
    append_format(out, " %u\n", static_cast<unsigned>(code[offset]));
    return offset + 1;
}

std::string disassemble_function(const ObjFunction& function) {
    std::string out;
    if (function.name != nullptr) {
        out += "== " + function.name->chars + " (arity " + std::to_string(function.arity) +
               ", upvalues " + std::to_string(function.upvalue_count) + ") ==\n";
    } else {
        out += "== <script> ==\n";
    }
    for (std::size_t offset = 0; offset < function.chunk.code.size();) {
        offset = disassemble_instruction(function.chunk, offset, out);
    }
    return out;
}

namespace {

void disassemble_tree(const ObjFunction& fn, std::string& out) {
    if (!out.empty()) out += '\n';
    out += disassemble_function(fn);
    for (Value v : fn.chunk.constants) {
        if (is_function(v)) disassemble_tree(*as_function(v), out);
    }
}

}  // namespace

std::string disassemble(const ObjFunction& script) {
    std::string out;
    disassemble_tree(script, out);
    return out;
}

// ---- Register bytecode ------------------------------------------------------------------------
// Same row layout as above; `r3` is register 3 and `k2(7)` constant 2, shown by value.

namespace {

const char* reg_op_name(RegOp op) {
    switch (op) {
        case RegOp::Move: return "MOVE";
        case RegOp::LoadK: return "LOADK";
        case RegOp::LoadNil: return "LOADNIL";
        case RegOp::LoadTrue: return "LOADTRUE";
        case RegOp::LoadFalse: return "LOADFALSE";
        case RegOp::GetGlobal: return "GET_GLOBAL";
        case RegOp::SetGlobal: return "SET_GLOBAL";
        case RegOp::DefineGlobal: return "DEFINE_GLOBAL";
        case RegOp::GetUpvalue: return "GET_UPVALUE";
        case RegOp::SetUpvalue: return "SET_UPVALUE";
        case RegOp::Add: return "ADD";
        case RegOp::Sub: return "SUB";
        case RegOp::Mul: return "MUL";
        case RegOp::Div: return "DIV";
        case RegOp::Mod: return "MOD";
        case RegOp::Eq: return "EQ";
        case RegOp::Ne: return "NE";
        case RegOp::Lt: return "LT";
        case RegOp::Le: return "LE";
        case RegOp::Gt: return "GT";
        case RegOp::Ge: return "GE";
        case RegOp::Neg: return "NEG";
        case RegOp::Not: return "NOT";
        case RegOp::Jump: return "JUMP";
        case RegOp::JumpIfFalse: return "JUMP_IF_FALSE";
        case RegOp::JumpIfTrue: return "JUMP_IF_TRUE";
        case RegOp::Call: return "CALL";
        case RegOp::Closure: return "CLOSURE";
        case RegOp::Capture: return "CAPTURE";
        case RegOp::Close: return "CLOSE";
        case RegOp::Return: return "RETURN";
        case RegOp::ReturnNil: return "RETURN_NIL";
        case RegOp::Print: return "PRINT";
        case RegOp::Array: return "ARRAY";
        case RegOp::ArrayAppend: return "ARRAY_APPEND";
        case RegOp::IndexGet: return "INDEX_GET";
        case RegOp::IndexSet: return "INDEX_SET";
    }
    return "UNKNOWN";
}

std::string reg_text(std::uint32_t reg) { return "r" + std::to_string(reg); }

// `k3(7)`: constant 3, shown by value.
std::string const_text(const RegChunk& chunk, std::uint32_t index) {
    std::string out = "k" + std::to_string(index) + "(";
    out += index < chunk.constants.size() ? constant_text(chunk.constants[index])
                                          : "<bad constant>";
    return out + ")";
}

// An RK operand: a register or, when its flag is set, a constant.
std::string rk_text(const RegChunk& chunk, std::uint32_t operand, bool is_const) {
    return is_const ? const_text(chunk, operand) : reg_text(operand);
}

}  // namespace

std::size_t disassemble_register_instruction(const RegChunk& chunk, std::size_t index,
                                             std::string& out) {
    Instruction insn = chunk.code[index];
    RegOp op = insn_op(insn);
    std::uint32_t a = insn_a(insn);
    std::uint32_t b = insn_b(insn);
    std::uint32_t c = insn_c(insn);
    bool b_const = (insn_flags(insn) & kFlagBConst) != 0;
    bool c_const = (insn_flags(insn) & kFlagCConst) != 0;
    const char* name = reg_op_name(op);
    std::size_t next = index + 1;

    append_format(out, "%04zu %4d ", index, chunk.line_at(index));
    std::string operands;
    switch (op) {
        case RegOp::ReturnNil:
            out += name;
            out += '\n';
            return next;
        case RegOp::Move: operands = reg_text(a) + " " + reg_text(b); break;
        case RegOp::LoadK: operands = reg_text(a) + " " + const_text(chunk, insn_bx(insn)); break;
        case RegOp::LoadNil:
        case RegOp::LoadTrue:
        case RegOp::LoadFalse:
        case RegOp::Close: operands = reg_text(a); break;
        case RegOp::GetGlobal:
        case RegOp::SetGlobal:
        case RegOp::DefineGlobal:
            operands = reg_text(a) + " " + const_text(chunk, insn_bx(insn));
            break;
        case RegOp::GetUpvalue:
        case RegOp::SetUpvalue: operands = reg_text(a) + " " + std::to_string(b); break;
        case RegOp::Add:
        case RegOp::Sub:
        case RegOp::Mul:
        case RegOp::Div:
        case RegOp::Mod:
        case RegOp::Eq:
        case RegOp::Ne:
        case RegOp::Lt:
        case RegOp::Le:
        case RegOp::Gt:
        case RegOp::Ge:
        case RegOp::IndexGet:
        case RegOp::IndexSet:
            operands = reg_text(a) + " " + rk_text(chunk, b, b_const) + " " +
                       rk_text(chunk, c, c_const);
            break;
        case RegOp::Neg:
        case RegOp::Not: operands = reg_text(a) + " " + rk_text(chunk, b, b_const); break;
        case RegOp::Return:
        case RegOp::Print: operands = rk_text(chunk, b, b_const); break;
        case RegOp::Jump:
        case RegOp::JumpIfFalse:
        case RegOp::JumpIfTrue: {
            // The offset counts from the next instruction (see RegOp::Jump).
            long long target = static_cast<long long>(next) + insn_sbx(insn);
            if (op != RegOp::Jump) operands = reg_text(a) + " ";
            char buf[32];
            std::snprintf(buf, sizeof buf, "-> %04lld", target);
            operands += buf;
            break;
        }
        case RegOp::Call: operands = reg_text(a) + " " + std::to_string(b); break;
        case RegOp::Array:
        case RegOp::ArrayAppend:
            operands = reg_text(a) + " " + reg_text(b) + " " + std::to_string(c);
            break;
        case RegOp::Capture:
            // Only ever printed under its CLOSURE row, below; a stray one shows its fields.
            operands = std::to_string(a) + " " + std::to_string(b);
            break;
        case RegOp::Closure: {
            std::uint32_t k = insn_bx(insn);
            bool is_fn = k < chunk.constants.size() && is_function(chunk.constants[k]);
            append_format(out, "%-14s %s %s\n", name, reg_text(a).c_str(),
                          const_text(chunk, k).c_str());
            if (!is_fn) return next;
            int count = as_function(chunk.constants[k])->upvalue_count;
            for (int i = 0; i < count; ++i) {
                if (next >= chunk.code.size()) {
                    out += "     <truncated>\n";
                    return chunk.code.size();
                }
                Instruction cap = chunk.code[next];
                // The Capture words are operands of CLOSURE, shown at their own indices.
                append_format(out, "%04zu %4d %-14s %s %u\n", next, chunk.line_at(next), "  |",
                              insn_a(cap) != 0 ? "local" : "upvalue",
                              static_cast<unsigned>(insn_b(cap)));
                ++next;
            }
            return next;
        }
    }
    append_format(out, "%-14s %s\n", name, operands.c_str());
    return next;
}

std::string disassemble_register_function(const ObjFunction& function) {
    std::string out;
    if (function.name != nullptr) {
        out += "== " + function.name->chars + " (arity " + std::to_string(function.arity) +
               ", upvalues " + std::to_string(function.upvalue_count) + ", frame " +
               std::to_string(function.reg.frame_size) + ") ==\n";
    } else {
        out += "== <script> (frame " + std::to_string(function.reg.frame_size) + ") ==\n";
    }
    for (std::size_t index = 0; index < function.reg.code.size();) {
        index = disassemble_register_instruction(function.reg, index, out);
    }
    return out;
}

namespace {

void disassemble_register_tree(const ObjFunction& fn, std::string& out) {
    if (!out.empty()) out += '\n';
    out += disassemble_register_function(fn);
    for (Value v : fn.reg.constants) {
        if (is_function(v)) disassemble_register_tree(*as_function(v), out);
    }
}

}  // namespace

std::string disassemble_register(const ObjFunction& script) {
    std::string out;
    disassemble_register_tree(script, out);
    return out;
}

}  // namespace rung
