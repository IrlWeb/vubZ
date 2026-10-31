#pragma once

#include <Scripting/Modules/FunctionBytecode.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace Disassembler
{
    struct Options
    {
        bool recursive = true;
        bool showLines = true;
        bool showConstants = true;
        int stringPreviewMax = 64;
    };

    inline const char* op_name(int op)
    {
        switch (op)
        {
#define OP(name) case LOP_##name: return #name;
        OP(NOP) OP(BREAK) OP(LOADNIL) OP(LOADB) OP(LOADN) OP(LOADK) OP(MOVE)
        OP(GETGLOBAL) OP(SETGLOBAL) OP(GETUPVAL) OP(SETUPVAL) OP(CLOSEUPVALS)
        OP(GETIMPORT) OP(GETTABLE) OP(SETTABLE) OP(GETTABLEKS) OP(SETTABLEKS)
        OP(GETTABLEN) OP(SETTABLEN) OP(NEWCLOSURE) OP(NAMECALL) OP(CALL) OP(RETURN)
        OP(JUMP) OP(JUMPBACK) OP(JUMPIF) OP(JUMPIFNOT) OP(JUMPIFEQ) OP(JUMPIFLE)
        OP(JUMPIFLT) OP(JUMPIFNOTEQ) OP(JUMPIFNOTLE) OP(JUMPIFNOTLT)
        OP(ADD) OP(SUB) OP(MUL) OP(DIV) OP(MOD) OP(POW)
        OP(ADDK) OP(SUBK) OP(MULK) OP(DIVK) OP(MODK) OP(POWK)
        OP(AND) OP(OR) OP(ANDK) OP(ORK) OP(CONCAT) OP(NOT) OP(MINUS) OP(LENGTH)
        OP(NEWTABLE) OP(DUPTABLE) OP(SETLIST) OP(FORNPREP) OP(FORNLOOP) OP(FORGLOOP)
        OP(FORGPREP_INEXT) OP(FASTCALL3) OP(FORGPREP_NEXT) OP(NATIVECALL)
        OP(GETVARARGS) OP(DUPCLOSURE) OP(PREPVARARGS) OP(LOADKX) OP(JUMPX)
        OP(FASTCALL) OP(COVERAGE) OP(CAPTURE) OP(SUBRK) OP(DIVRK)
        OP(FASTCALL1) OP(FASTCALL2) OP(FASTCALL2K) OP(FORGPREP)
        OP(JUMPXEQKNIL) OP(JUMPXEQKB) OP(JUMPXEQKN) OP(JUMPXEQKS)
        OP(IDIV) OP(IDIVK) OP(GETUDATAKS) OP(SETUDATAKS) OP(NAMECALLUDATA)
        OP(NEWCLASSMEMBER) OP(CALLFB) OP(CMPPROTO) OP(NEWCLASS)
#undef OP
        default: return "UNKNOWN";
        }
    }

    struct Writer
    {
        lua_State* L;
        const Options& options;
        std::string out;
        FunctionBytecode::Builder decoder{ L };

        void line(const std::string& text)
        {
            out += text;
            out.push_back('\n');
        }

        static std::string format(const char* fmt, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buffer, sizeof(buffer), fmt, args);
            va_end(args);
            return buffer;
        }

        std::string quote(const char* data, size_t length) const
        {
            const size_t limit = options.stringPreviewMax > 0 ? static_cast<size_t>(options.stringPreviewMax) : length;
            std::string text = "\"";
            for (size_t i = 0; i < length && i < limit; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(data[i]);
                switch (c)
                {
                case '"': text += "\\\""; break;
                case '\\': text += "\\\\"; break;
                case '\n': text += "\\n"; break;
                case '\r': text += "\\r"; break;
                case '\t': text += "\\t"; break;
                default:
                    if (c < 0x20 || c >= 0x7F)
                        text += format("\\x%02X", c);
                    else
                        text.push_back(static_cast<char>(c));
                }
            }
            text += length > limit ? "\"..." : "\"";
            return text;
        }

        static std::string number(double value)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value > 0 ? "inf" : "-inf";
            return format("%.17g", value);
        }

        std::string string_constant(Proto* p, uint32_t index) const
        {
            if (index >= static_cast<uint32_t>(p->sizek) || !ttisstring(&p->k[index]))
                return format("K%u", index);
            const TString* ts = tsvalue(&p->k[index]);
            return quote(getstr(ts), ts->len);
        }

        std::string import_path(Proto* p, uint32_t aux) const
        {
            const int count = static_cast<int>(aux >> 30);
            const uint32_t ids[3] = { (aux >> 20) & 1023, (aux >> 10) & 1023, aux & 1023 };
            std::string path;
            for (int i = 0; i < count && i < 3; ++i)
            {
                if (i)
                    path.push_back('.');
                if (ids[i] < static_cast<uint32_t>(p->sizek) && ttisstring(&p->k[ids[i]]))
                {
                    const TString* ts = tsvalue(&p->k[ids[i]]);
                    path.append(getstr(ts), ts->len);
                }
                else
                {
                    path += "?";
                }
            }
            return path;
        }

        std::string constant(Proto* p, int index, const std::vector<uint32_t>& imports) const
        {
            if (index < 0 || index >= p->sizek)
                return "?";
            if (imports[index])
                return "import " + import_path(p, imports[index]);

            const TValue* k = &p->k[index];
            switch (ttype(k))
            {
            case LUA_TNIL: return "nil";
            case LUA_TBOOLEAN: return bvalue(k) ? "true" : "false";
            case LUA_TNUMBER: return number(nvalue(k));
            case LUA_TINTEGER: return format("%lldi", static_cast<long long>(lvalue(k)));
            case LUA_TSTRING: return quote(getstr(tsvalue(k)), tsvalue(k)->len);
            case LUA_TVECTOR:
            {
                const float* v = vvalue(k);
                return format("vector(%g, %g, %g)", v[0], v[1], v[2]);
            }
            case LUA_TTABLE: return "{...}";
            case LUA_TFUNCTION:
            {
                const Closure* cl = clvalue(k);
                if (!cl->isC && cl->l.p)
                {
                    TString* name = cl->l.p->debugname.Get();
                    return name ? std::string("function ") + getstr(name) : "function <anonymous>";
                }
                return "function [C]";
            }
            default: return lua_typename(L, ttype(k));
            }
        }

        static int line_of(Proto* p, int pc)
        {
            const unsigned char* lineinfo = p->lineinfo.Get();
            const int* abslineinfo = p->abslineinfo.Get();
            if (!lineinfo || !abslineinfo)
                return 0;
            return abslineinfo[pc >> p->linegaplog2] + lineinfo[pc];
        }

        std::string operands(Proto* p, const std::vector<uint32_t>& code, const std::vector<uint32_t>& imports,
            int pc, int op, std::string& comment)
        {
            const uint32_t insn = code[pc];
            const uint32_t aux = pc + 1 < static_cast<int>(code.size()) ? code[pc + 1] : 0;
            const int A = LUAU_INSN_A(insn);
            const int B = LUAU_INSN_B(insn);
            const int C = LUAU_INSN_C(insn);
            const int D = LUAU_INSN_D(insn);
            const int E = LUAU_INSN_E(insn);
            auto label = [&](int offset) { return format("L%d", pc + 1 + offset); };

            switch (op)
            {
            case LOP_LOADNIL: case LOP_CLOSEUPVALS: case LOP_PREPVARARGS:
                return format("R%d", A);
            case LOP_LOADB:
                return C ? format("R%d %s +%d", A, B ? "true" : "false", C) : format("R%d %s", A, B ? "true" : "false");
            case LOP_LOADN:
                return format("R%d %d", A, D);
            case LOP_LOADK:
                comment = constant(p, D, imports);
                return format("R%d K%d", A, D);
            case LOP_LOADKX:
                comment = constant(p, static_cast<int>(aux), imports);
                return format("R%d K%u", A, aux);
            case LOP_MOVE: case LOP_NOT: case LOP_MINUS: case LOP_LENGTH:
                return format("R%d R%d", A, B);
            case LOP_GETGLOBAL: case LOP_SETGLOBAL:
                comment = string_constant(p, aux);
                return format("R%d K%u", A, aux);
            case LOP_GETUPVAL: case LOP_SETUPVAL:
                return format("R%d U%d", A, B);
            case LOP_GETIMPORT:
                comment = import_path(p, aux);
                return format("R%d K%d", A, D);
            case LOP_GETTABLE: case LOP_SETTABLE:
            case LOP_ADD: case LOP_SUB: case LOP_MUL: case LOP_DIV: case LOP_IDIV: case LOP_MOD: case LOP_POW:
            case LOP_AND: case LOP_OR:
                return format("R%d R%d R%d", A, B, C);
            case LOP_ADDK: case LOP_SUBK: case LOP_MULK: case LOP_DIVK: case LOP_IDIVK: case LOP_MODK: case LOP_POWK:
            case LOP_ANDK: case LOP_ORK:
                comment = constant(p, C, imports);
                return format("R%d R%d K%d", A, B, C);
            case LOP_SUBRK: case LOP_DIVRK:
                comment = constant(p, B, imports);
                return format("R%d K%d R%d", A, B, C);
            case LOP_GETTABLEKS: case LOP_SETTABLEKS: case LOP_NAMECALL:
                comment = string_constant(p, aux);
                return format("R%d R%d K%u", A, B, aux);
            case LOP_GETTABLEN: case LOP_SETTABLEN:
                return format("R%d R%d [%d]", A, B, C + 1);
            case LOP_NEWCLOSURE:
                return format("R%d P%d", A, D);
            case LOP_DUPCLOSURE:
            case LOP_DUPTABLE:
                comment = constant(p, D, imports);
                return format("R%d K%d", A, D);
            case LOP_CALL:
                comment = format("%s args, %s results",
                    B ? std::to_string(B - 1).c_str() : "multiple",
                    C ? std::to_string(C - 1).c_str() : "multiple");
                return format("R%d %d %d", A, B, C);
            case LOP_RETURN:
                comment = B ? format("%d values", B - 1) : "multiple values";
                return format("R%d %d", A, B);
            case LOP_GETVARARGS:
                return format("R%d %d", A, B);
            case LOP_JUMP: case LOP_JUMPBACK:
                return label(D);
            case LOP_JUMPX:
                return label(E);
            case LOP_JUMPIF: case LOP_JUMPIFNOT:
            case LOP_FORNPREP: case LOP_FORNLOOP:
            case LOP_FORGPREP: case LOP_FORGPREP_INEXT: case LOP_FORGPREP_NEXT:
                return format("R%d %s", A, label(D).c_str());
            case LOP_FORGLOOP:
                comment = format("%d vars%s", static_cast<int>(aux & 0xFF), (aux >> 31) ? ", ipairs" : "");
                return format("R%d %s", A, label(D).c_str());
            case LOP_JUMPIFEQ: case LOP_JUMPIFLE: case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ: case LOP_JUMPIFNOTLE: case LOP_JUMPIFNOTLT:
                return format("R%d R%u %s", A, aux, label(D).c_str());
            case LOP_JUMPXEQKNIL:
                return format("R%d %s%s", A, label(D).c_str(), LUAU_INSN_AUX_NOT(aux) ? " NOT" : "");
            case LOP_JUMPXEQKB:
                return format("R%d %s %s%s", A, LUAU_INSN_AUX_KB(aux) ? "true" : "false",
                    label(D).c_str(), LUAU_INSN_AUX_NOT(aux) ? " NOT" : "");
            case LOP_JUMPXEQKN: case LOP_JUMPXEQKS:
                comment = constant(p, static_cast<int>(LUAU_INSN_AUX_KV(aux)), imports);
                return format("R%d K%u %s%s", A, LUAU_INSN_AUX_KV(aux), label(D).c_str(),
                    LUAU_INSN_AUX_NOT(aux) ? " NOT" : "");
            case LOP_CONCAT:
                return format("R%d R%d..R%d", A, B, C);
            case LOP_NEWTABLE:
                comment = format("hash %d, array %u", B ? (1 << (B - 1)) : 0, aux);
                return format("R%d %d %u", A, B, aux);
            case LOP_SETLIST:
                comment = C ? format("%d values from index %u", C - 1, aux) : format("multiple values from index %u", aux);
                return format("R%d R%d %d %u", A, B, C, aux);
            case LOP_CAPTURE:
            {
                static const char* kinds[] = { "VAL", "REF", "UPVAL" };
                return format("%s %s%d", A < 3 ? kinds[A] : "?", A == 2 ? "U" : "R", B);
            }
            case LOP_FASTCALL:
                return format("%d %s", A, label(C).c_str());
            case LOP_FASTCALL1:
                return format("%d R%d %s", A, B, label(C).c_str());
            case LOP_FASTCALL2:
                return format("%d R%d R%u %s", A, B, aux & 0xFF, label(C).c_str());
            case LOP_FASTCALL2K:
                comment = constant(p, static_cast<int>(aux), imports);
                return format("%d R%d K%u %s", A, B, aux, label(C).c_str());
            case LOP_FASTCALL3:
                return format("%d R%d R%u R%u %s", A, B, aux & 0xFF, (aux >> 8) & 0xFF, label(C).c_str());
            case LOP_COVERAGE:
                return format("%d", E);
            case LOP_NOP: case LOP_BREAK: case LOP_NATIVECALL:
                return "";
            default:
                if (Luau::getOpLength(static_cast<LuauOpcode>(op)) > 1)
                    return format("%d %d %d [%u]", A, B, C, aux);
                return format("%d %d %d", A, B, C);
            }
        }

        bool proto(Proto* p, int depth, int id)
        {
            std::vector<uint32_t> code;
            std::vector<uint32_t> imports;
            if (!decoder.decode(p, code, imports))
                return false;

            TString* name = p->debugname.Get();
            std::string header = format("; P%d function %s(", id, name ? getstr(name) : "<anonymous>");
            for (int i = 0; i < p->numparams; ++i)
                header += format(i ? ", a%d" : "a%d", i);
            if (p->is_vararg)
                header += p->numparams ? ", ..." : "...";
            header += format(") line %d, %d upvalues, stack %d", p->linedefined, p->nups, p->maxstacksize);
            line(header);

            if (options.showConstants && p->sizek > 0)
            {
                line(format("; constants (%d)", p->sizek));
                for (int i = 0; i < p->sizek; ++i)
                    line(format(";   K%d = %s", i, constant(p, i, imports).c_str()));
            }

            for (int pc = 0; pc < static_cast<int>(code.size());)
            {
                const int op = LUAU_INSN_OP(code[pc]);
                std::string comment;
                const std::string args = operands(p, code, imports, pc, op, comment);

                std::string text = options.showLines ? format("[%4d] ", line_of(p, pc)) : "";
                text += format("L%-5d %-15s %s", pc, op_name(op), args.c_str());
                if (!comment.empty())
                {
                    while (text.size() < 48)
                        text.push_back(' ');
                    text += " ; " + comment;
                }
                line(text);

                const int length = Luau::getOpLength(static_cast<LuauOpcode>(op));
                pc += length > 0 ? length : 1;
            }

            if (options.recursive)
            {
                for (int i = 0; i < p->sizep; ++i)
                {
                    line("");
                    if (!proto(p->p[i], depth + 1, i))
                        return false;
                }
            }
            return true;
        }
    };

    inline Options read_options(lua_State* L, int index)
    {
        Options options;
        if (!lua_istable(L, index))
            return options;

        auto flag = [&](const char* field, bool& target)
        {
            lua_getfield(L, index, field);
            if (lua_isboolean(L, -1))
                target = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        };
        flag("Recursive", options.recursive);
        flag("ShowLines", options.showLines);
        flag("ShowConstants", options.showConstants);

        lua_getfield(L, index, "StringPreviewMax");
        if (lua_isnumber(L, -1))
            options.stringPreviewMax = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return options;
    }

    inline int disassemble(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        const Closure* cl = clvalue(luaA_toobject(L, 1));
        if (cl->isC)
            luaL_error(L, "cannot disassemble C closure");
        if (!cl->l.p)
            luaL_error(L, "invalid closure");
        if (!FunctionBytecode::build_decode_table())
            luaL_error(L, "disassemble: opcode table unavailable");

        const Options options = read_options(L, 2);
        Writer writer{ L, options };
        if (!writer.proto(cl->l.p, 0, 0))
            luaL_error(L, "disassemble: %s", writer.decoder.error.c_str());

        if (writer.out.empty() || writer.out.back() != '\n')
            writer.out.push_back('\n');
        while (writer.out.size() >= 2 && writer.out[writer.out.size() - 2] == '\n')
            writer.out.pop_back();

        lua_pushlstring(L, writer.out.data(), writer.out.size());
        return 1;
    }
}
