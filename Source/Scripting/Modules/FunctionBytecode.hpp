#pragma once

#include <lstate.h>
#include <lobject.h>
#include <lapi.h>
#include <lualib.h>
#include <Luau/Bytecode.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Update/Offsets.hpp"

namespace FunctionBytecode
{
    constexpr uint8_t Version = 8;
    constexpr uint8_t TypesVersion = 1;

    inline uint8_t g_decode[256];
    inline bool g_decodeReady = false;

    inline bool build_decode_table()
    {
        if (g_decodeReady)
            return true;

        const auto table = reinterpret_cast<const uint8_t*>(Offsets::OpcodeLookupTable);
        if (!table)
            return false;

        std::memset(g_decode, 0xFF, sizeof(g_decode));
        for (int op = 0; op < LOP__COUNT; ++op)
        {
            const uint8_t internal = table[static_cast<uint8_t>(op * 227)];
            if (g_decode[internal] != 0xFF)
                return false;
            g_decode[internal] = static_cast<uint8_t>(op);
        }

        g_decodeReady = true;
        return true;
    }

    struct Writer
    {
        std::string out;

        void byte(uint8_t value) { out.push_back(static_cast<char>(value)); }

        void varint(uint64_t value)
        {
            do
            {
                uint8_t chunk = value & 0x7F;
                value >>= 7;
                byte(chunk | (value ? 0x80 : 0));
            } while (value);
        }

        void u32(uint32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); }
        void i32(int32_t value) { out.append(reinterpret_cast<const char*>(&value), 4); }
        void f32(float value) { out.append(reinterpret_cast<const char*>(&value), 4); }
        void f64(double value) { out.append(reinterpret_cast<const char*>(&value), 8); }
        void raw(const std::string& data) { out.append(data); }
    };

    struct Builder
    {
        lua_State* L;
        std::vector<Proto*> order;
        std::unordered_map<Proto*, uint32_t> ids;
        std::vector<std::string> strings;
        std::unordered_map<std::string, uint32_t> stringIds;
        std::string error;

        uint32_t string_id(TString* ts)
        {
            if (!ts)
                return 0;

            std::string value(getstr(ts), ts->len);
            auto it = stringIds.find(value);
            if (it != stringIds.end())
                return it->second;

            strings.push_back(value);
            const uint32_t id = static_cast<uint32_t>(strings.size());
            stringIds.emplace(std::move(value), id);
            return id;
        }

        void collect(Proto* p)
        {
            if (!p || ids.count(p))
                return;

            for (int i = 0; i < p->sizep; ++i)
                collect(p->p[i]);

            ids[p] = static_cast<uint32_t>(order.size());
            order.push_back(p);
        }

        bool decode(Proto* p, std::vector<uint32_t>& code, std::vector<uint32_t>& imports)
        {
            code.assign(p->code, p->code + p->sizecode);
            imports.assign(p->sizek, 0);

            for (int pc = 0; pc < p->sizecode;)
            {
                const uint8_t internal = LUAU_INSN_OP(code[pc]);
                uint8_t op = g_decode[internal];
                if (op == 0xFF)
                {
                    error = "unknown opcode in proto";
                    return false;
                }

                const bool hasAux = pc + 1 < p->sizecode;

                switch (op)
                {
                case LOP_GETUDATAKS:
                case LOP_SETUDATAKS:
                case LOP_NAMECALLUDATA:
                    op = op == LOP_GETUDATAKS ? LOP_GETTABLEKS
                        : op == LOP_SETUDATAKS ? LOP_SETTABLEKS
                        : LOP_NAMECALL;
                    if (hasAux)
                        code[pc + 1] &= 0xFFFF;
                    break;

                case LOP_CALLFB:
                    code[pc] = (code[pc] & ~0xFFu) | LOP_CALL;
                    if (hasAux)
                        code[pc + 1] = LOP_NOP;
                    pc += 2;
                    continue;

                case LOP_GETIMPORT:
                {
                    const int k = LUAU_INSN_D(code[pc]);
                    if (hasAux && k >= 0 && k < p->sizek)
                        imports[k] = code[pc + 1];
                    break;
                }

                default:
                    break;
                }

                code[pc] = (code[pc] & ~0xFFu) | op;

                const int length = Luau::getOpLength(static_cast<LuauOpcode>(op));
                if (length <= 0)
                {
                    error = "bad opcode length";
                    return false;
                }
                pc += length;
            }

            return true;
        }

        int find_constant(Proto* p, int value)
        {
            value = lua_absindex(L, value);
            for (int i = 0; i < p->sizek; ++i)
            {
                const TValue* k = &p->k[i];
                if (ttisnil(k) || ttistable(k) || ttisfunction(k))
                    continue;

                luaC_threadbarrier(L);
                luaA_pushvalue(L, k);
                const bool same = lua_rawequal(L, -1, value) != 0;
                lua_pop(L, 1);
                if (same)
                    return i;
            }
            return -1;
        }

        void write_table(Writer& w, Proto* p, const TValue* k)
        {
            struct Entry { int key; int value; };
            std::vector<Entry> entries;

            luaC_threadbarrier(L);
            luaA_pushvalue(L, k);
            const int table = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, table) != 0)
            {
                const int key = find_constant(p, -2);
                const int value = lua_isnil(L, -1) ? -1 : find_constant(p, -1);
                if (key >= 0)
                    entries.push_back({ key, value });
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            w.byte(LBC_CONSTANT_TABLE_WITH_CONSTANTS);
            w.varint(entries.size());
            for (const Entry& entry : entries)
            {
                w.varint(static_cast<uint32_t>(entry.key));
                w.i32(entry.value);
            }
        }

        bool write_constants(Writer& w, Proto* p, const std::vector<uint32_t>& imports)
        {
            w.varint(p->sizek);
            for (int i = 0; i < p->sizek; ++i)
            {
                const TValue* k = &p->k[i];

                if (imports[i])
                {
                    w.byte(LBC_CONSTANT_IMPORT);
                    w.u32(imports[i]);
                    continue;
                }

                switch (ttype(k))
                {
                case LUA_TNIL:
                    w.byte(LBC_CONSTANT_NIL);
                    break;
                case LUA_TBOOLEAN:
                    w.byte(LBC_CONSTANT_BOOLEAN);
                    w.byte(bvalue(k) ? 1 : 0);
                    break;
                case LUA_TNUMBER:
                    w.byte(LBC_CONSTANT_NUMBER);
                    w.f64(nvalue(k));
                    break;
                case LUA_TINTEGER:
                {
                    const int64_t value = lvalue(k);
                    w.byte(LBC_CONSTANT_INTEGER);
                    w.byte(value < 0 ? 1 : 0);
                    w.varint(value < 0 ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value));
                    break;
                }
                case LUA_TSTRING:
                    w.byte(LBC_CONSTANT_STRING);
                    w.varint(string_id(tsvalue(k)));
                    break;
                case LUA_TVECTOR:
                {
                    const float* v = vvalue(k);
                    w.byte(LBC_CONSTANT_VECTOR);
                    w.f32(v[0]);
                    w.f32(v[1]);
                    w.f32(v[2]);
                    w.f32(0.0f);
                    break;
                }
                case LUA_TTABLE:
                    write_table(w, p, k);
                    break;
                case LUA_TFUNCTION:
                {
                    Closure* cl = clvalue(k);
                    auto it = (!cl->isC && cl->l.p) ? ids.find(cl->l.p) : ids.end();
                    if (it == ids.end())
                    {
                        w.byte(LBC_CONSTANT_NIL);
                        break;
                    }
                    w.byte(LBC_CONSTANT_CLOSURE);
                    w.varint(it->second);
                    break;
                }
                default:
                    w.byte(LBC_CONSTANT_NIL);
                    break;
                }
            }
            return true;
        }

        bool write_proto(Writer& w, Proto* p)
        {
            std::vector<uint32_t> code;
            std::vector<uint32_t> imports;
            if (!decode(p, code, imports))
                return false;

            w.byte(p->maxstacksize);
            w.byte(p->numparams);
            w.byte(p->nups);
            w.byte(p->is_vararg);

            w.byte(0);
            w.varint(0);

            w.varint(code.size());
            for (uint32_t insn : code)
                w.u32(insn);

            if (!write_constants(w, p, imports))
                return false;

            w.varint(p->sizep);
            for (int i = 0; i < p->sizep; ++i)
                w.varint(ids[p->p[i]]);

            w.varint(p->linedefined);
            w.varint(string_id(p->debugname.Get()));

            const unsigned char* lineinfo = p->lineinfo.Get();
            const int* abslineinfo = p->abslineinfo.Get();
            if (lineinfo && abslineinfo && p->sizecode > 0)
            {
                w.byte(1);
                w.byte(static_cast<uint8_t>(p->linegaplog2));

                uint8_t last = 0;
                for (int i = 0; i < p->sizecode; ++i)
                {
                    w.byte(static_cast<uint8_t>(lineinfo[i] - last));
                    last = lineinfo[i];
                }

                const int intervals = ((p->sizecode - 1) >> p->linegaplog2) + 1;
                int lastLine = 0;
                for (int i = 0; i < intervals; ++i)
                {
                    w.i32(abslineinfo[i] - lastLine);
                    lastLine = abslineinfo[i];
                }
            }
            else
            {
                w.byte(0);
            }

            const LocVar* locvars = p->locvars.Get();
            TString** upvalues = p->upvalues.Get();
            const bool debug = (locvars && p->sizelocvars > 0) || (upvalues && p->sizeupvalues > 0);
            w.byte(debug ? 1 : 0);
            if (debug)
            {
                const int locCount = locvars ? p->sizelocvars : 0;
                w.varint(locCount);
                for (int i = 0; i < locCount; ++i)
                {
                    w.varint(string_id(locvars[i].varname));
                    w.varint(locvars[i].startpc);
                    w.varint(locvars[i].endpc);
                    w.byte(locvars[i].reg);
                }

                const int upCount = upvalues ? p->sizeupvalues : 0;
                w.varint(upCount);
                for (int i = 0; i < upCount; ++i)
                    w.varint(string_id(upvalues[i]));
            }

            return true;
        }
    };

    inline bool Serialize(lua_State* L, Proto* root, std::string& output, std::string& error)
    {
        if (!build_decode_table())
        {
            error = "opcode table unavailable";
            return false;
        }

        Builder builder{ L };
        builder.collect(root);

        Writer protos;
        protos.varint(builder.order.size());
        for (Proto* p : builder.order)
        {
            if (!builder.write_proto(protos, p))
            {
                error = builder.error.empty() ? "failed to serialise proto" : builder.error;
                return false;
            }
        }
        protos.varint(builder.ids[root]);

        Writer header;
        header.byte(Version);
        header.byte(TypesVersion);
        header.varint(builder.strings.size());
        for (const std::string& value : builder.strings)
        {
            header.varint(value.size());
            header.raw(value);
        }

        output = header.out + protos.out;
        return true;
    }

    inline int getfunctionbytecode(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        Closure* cl = clvalue(luaA_toobject(L, 1));
        if (cl->isC || !cl->l.p)
            luaL_argerror(L, 1, "Luau function expected");

        std::string bytecode;
        std::string error;
        if (!Serialize(L, cl->l.p, bytecode, error))
            luaL_error(L, "getfunctionbytecode: %s", error.c_str());

        lua_pushlstring(L, bytecode.data(), bytecode.size());
        return 1;
    }
}
