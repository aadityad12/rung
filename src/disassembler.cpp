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

}  // namespace rung
