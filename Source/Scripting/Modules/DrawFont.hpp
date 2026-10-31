#pragma once

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <lstate.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Core/Utilities.hpp>
#include <Ui/Overlay/WindowOverlay.hpp>

namespace DrawFont
{
    inline const char* const MetatableName = "DrawFont";
    inline char FontsKey;

    struct Font
    {
        std::vector<unsigned char> data;
        stbtt_fontinfo info{};
        float pixelSize = 0.0f;
    };

    inline std::mutex g_lock;
    inline std::unordered_map<int, std::unique_ptr<Font>> g_fonts;
    inline int g_nextId = 1;

    struct Ref
    {
        int id;
    };

    inline int To(lua_State* L, int index)
    {
        if (lua_type(L, index) != LUA_TUSERDATA || !lua_getmetatable(L, index))
            return 0;

        luaL_getmetatable(L, MetatableName);
        const bool ours = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 2);
        return ours ? static_cast<Ref*>(lua_touserdata(L, index))->id : 0;
    }

    inline unsigned int NextCodepoint(const std::string& text, size_t& i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            return c;

        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (extra < 0)
            return 0xFFFD;

        unsigned int cp = c & (0x3F >> extra);
        while (extra-- > 0)
        {
            if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                return 0xFFFD;
            cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
        }
        return cp;
    }

    inline bool Measure(int id, float size, const std::string& text, float& width, float& height)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_fonts.find(id);
        if (it == g_fonts.end())
            return false;

        const stbtt_fontinfo& info = it->second->info;
        const float scale = stbtt_ScaleForPixelHeight(&info, size);

        float line = 0.0f;
        width = 0.0f;
        int lines = 1;
        for (size_t i = 0; i < text.size();)
        {
            const unsigned int cp = NextCodepoint(text, i);
            if (cp == '\n')
            {
                width = (std::max)(width, line);
                line = 0.0f;
                ++lines;
                continue;
            }
            int advance = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &advance, &bearing);
            line += advance * scale;
        }
        width = (std::max)(width, line);
        height = size * static_cast<float>(lines);
        return true;
    }

    inline void PushVector2(lua_State* L, float x, float y)
    {
        lua_getglobal(L, "Vector2");
        lua_getfield(L, -1, "new");
        lua_remove(L, -2);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_call(L, 2, 1);
    }

    inline int GetTextBounds(lua_State* L)
    {
        const int id = To(L, 1);
        if (!id)
            luaL_typeerrorL(L, 1, MetatableName);
        const float size = static_cast<float>(luaL_checknumber(L, 2));
        size_t length = 0;
        const char* text = luaL_checklstring(L, 3, &length);

        float width = 0.0f;
        float height = 0.0f;
        if (!Measure(id, size, std::string(text, length), width, height))
            luaL_error(L, "DrawFont is no longer available");

        PushVector2(L, std::ceil(width), height);
        return 1;
    }

    inline int Index(lua_State* L)
    {
        const char* key = luaL_checkstring(L, 2);
        if (strcmp(key, "GetTextBounds") == 0)
        {
            lua_pushcfunction(L, GetTextBounds, "GetTextBounds");
            return 1;
        }
        luaL_error(L, "%s is not a valid member of DrawFont", key);
        return 0;
    }

    inline int NewIndex(lua_State* L)
    {
        luaL_error(L, "DrawFont is read-only");
        return 0;
    }

    inline int ToString(lua_State* L)
    {
        lua_pushstring(L, "DrawFont");
        return 1;
    }

    inline std::vector<ImWchar> ReadGlyphs(lua_State* L, int options)
    {
        std::vector<unsigned int> points;
        lua_getfield(L, options, "Glyphs");
        if (lua_istable(L, -1))
        {
            const int glyphs = lua_gettop(L);
            const int groups = lua_objlen(L, glyphs);
            for (int g = 1; g <= groups; ++g)
            {
                lua_rawgeti(L, glyphs, g);
                if (lua_istable(L, -1))
                {
                    const int count = lua_objlen(L, -1);
                    for (int c = 1; c <= count; ++c)
                    {
                        lua_rawgeti(L, -1, c);
                        if (lua_isnumber(L, -1))
                        {
                            const double cp = lua_tonumber(L, -1);
                            if (cp >= 1 && cp <= 0xFFFF)
                                points.push_back(static_cast<unsigned int>(cp));
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        std::vector<ImWchar> ranges;
        if (points.empty())
            return ranges;

        std::sort(points.begin(), points.end());
        unsigned int start = points[0];
        unsigned int end = points[0];
        for (size_t i = 1; i <= points.size(); ++i)
        {
            if (i < points.size() && points[i] <= end + 1)
            {
                end = (std::max)(end, points[i]);
                continue;
            }
            ranges.push_back(static_cast<ImWchar>(start));
            ranges.push_back(static_cast<ImWchar>(end));
            if (i < points.size())
                start = end = points[i];
        }
        ranges.push_back(0);
        return ranges;
    }

    inline void PushFont(lua_State* L, int id)
    {
        lua_pushlightuserdata(L, &FontsKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, -1, id);
        lua_remove(L, -2);
    }

    inline int Create(const char* data, size_t length, float pixelSize, std::vector<ImWchar> ranges)
    {
        auto font = std::make_unique<Font>();
        font->data.assign(reinterpret_cast<const unsigned char*>(data),
            reinterpret_cast<const unsigned char*>(data) + length);
        font->pixelSize = pixelSize;

        const int offset = length >= 12 ? stbtt_GetFontOffsetForIndex(font->data.data(), 0) : -1;
        if (offset < 0 || !stbtt_InitFont(&font->info, font->data.data(), offset))
            return 0;

        int id = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            id = g_nextId++;
            WindowOverlay::QueueFont(id, font->data, pixelSize, std::move(ranges));
            g_fonts.emplace(id, std::move(font));
        }
        WindowOverlay::Start();
        return id;
    }

    inline int Register(lua_State* L)
    {
        size_t length = 0;
        const char* data = luaL_checklstring(L, 1, &length);
        luaL_checktype(L, 2, LUA_TTABLE);

        lua_getfield(L, 2, "PixelSize");
        if (!lua_isnumber(L, -1))
            luaL_error(L, "DrawFont.Register: options.PixelSize is required");
        const float pixelSize = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (pixelSize < 1.0f || pixelSize > 256.0f)
            luaL_error(L, "DrawFont.Register: PixelSize must be between 1 and 256");

        const int id = Create(data, length, pixelSize, ReadGlyphs(L, 2));
        if (!id)
        {
            lua_pushnil(L);
            return 1;
        }

        auto* ref = static_cast<Ref*>(lua_newuserdata(L, sizeof(Ref)));
        ref->id = id;
        luaL_getmetatable(L, MetatableName);
        lua_setmetatable(L, -2);

        lua_pushlightuserdata(L, &FontsKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, id);
        lua_pop(L, 1);
        return 1;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        luaL_newmetatable(L, MetatableName);
        lua_pushcfunction(L, Index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, NewIndex, "__newindex");
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, ToString, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "DrawFont");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);

        lua_pushlightuserdata(L, &FontsKey);
        lua_newtable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);

        lua_newtable(L);
        Utils::AddTableFunction(L, "Register", Register);
        lua_setreadonly(L, -1, true);
        lua_setglobal(L, "DrawFont");
    }
}
