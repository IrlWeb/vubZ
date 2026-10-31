#pragma once

#include <lstate.h>
#include <lualib.h>
#include <lapi.h>

#include <cstring>
#include <string>
#include <unordered_map>

#include <Core/Utilities.hpp>
#include <Ui/Overlay/Renderer.hpp>
#include <Ui/Overlay/WindowOverlay.hpp>
#include <Scripting/Modules/DrawFont.hpp>
#include <Scripting/Modules/Signal.hpp>

#include <algorithm>

namespace Drawing
{
    static const char* const MetatableName = "DrawingObject";

    struct Ref
    {
        Renderer::Handle handle = Renderer::InvalidHandle;
    };

    inline std::unordered_map<Renderer::Handle, Renderer::Object> Tombstones;

    inline void ResetLibraryState()
    {
        Renderer::Reset();
        Renderer::ClearImmediate();
        Tombstones.clear();
    }

    inline Ref* to_ref(lua_State* L, int index, bool required)
    {
        void* raw = lua_touserdata(L, index);
        if (!raw)
        {
            if (required)
                luaL_argerror(L, index, "Drawing object expected");
            return nullptr;
        }

        if (!lua_getmetatable(L, index))
        {
            if (required)
                luaL_argerror(L, index, "Drawing object expected");
            return nullptr;
        }

        luaL_getmetatable(L, MetatableName);
        const bool ok = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 2);

        if (!ok)
        {
            if (required)
                luaL_argerror(L, index, "Drawing object expected");
            return nullptr;
        }

        return static_cast<Ref*>(raw);
    }

    inline void push_vector2(lua_State* L, const ImVec2& value)
    {
        lua_getglobal(L, "Vector2");
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "new");
            lua_remove(L, -2);
            lua_pushnumber(L, value.x);
            lua_pushnumber(L, value.y);
            if (lua_pcall(L, 2, 1, 0) == LUA_OK)
                return;
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
        }

        lua_pushnil(L);
    }

    inline ImVec2 to_vector2(lua_State* L, int index)
    {
        ImVec2 value{ 0.0f, 0.0f };
        if (!lua_isuserdata(L, index))
            return value;

        lua_getfield(L, index, "X");
        value.x = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, index, "Y");
        value.y = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        return value;
    }

    inline void push_color3(lua_State* L, const ImColor& value)
    {
        lua_getglobal(L, "Color3");
        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "new");
            lua_remove(L, -2);
            lua_pushnumber(L, value.Value.x);
            lua_pushnumber(L, value.Value.y);
            lua_pushnumber(L, value.Value.z);
            if (lua_pcall(L, 3, 1, 0) == LUA_OK)
                return;
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
        }

        lua_pushnil(L);
    }

    inline ImColor to_color3(lua_State* L, int index)
    {
        ImColor value(1.0f, 1.0f, 1.0f, 1.0f);
        if (!lua_isuserdata(L, index))
            return value;

        lua_getfield(L, index, "R");
        value.Value.x = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, index, "G");
        value.Value.y = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, index, "B");
        value.Value.z = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        return value;
    }

    inline bool get_property(lua_State* L, const Renderer::Object& o, const char* key)
    {
        using Kind = Renderer::Kind;

        if (std::strcmp(key, "Visible") == 0)      { lua_pushboolean(L, o.visible); return true; }
        if (std::strcmp(key, "ZIndex") == 0)       { lua_pushinteger(L, o.zindex); return true; }
        if (std::strcmp(key, "Transparency") == 0) { lua_pushnumber(L, o.transparency); return true; }
        if (std::strcmp(key, "Color") == 0)        { push_color3(L, o.color); return true; }
        if (std::strcmp(key, "Thickness") == 0)    { lua_pushnumber(L, o.thickness); return true; }
        if (std::strcmp(key, "Filled") == 0)       { lua_pushboolean(L, o.filled); return true; }
        if (std::strcmp(key, "From") == 0)         { push_vector2(L, o.from); return true; }
        if (std::strcmp(key, "To") == 0)           { push_vector2(L, o.to); return true; }
        if (std::strcmp(key, "Position") == 0)     { push_vector2(L, o.position); return true; }
        if (std::strcmp(key, "Radius") == 0)       { lua_pushnumber(L, o.radius); return true; }
        if (std::strcmp(key, "NumSides") == 0)     { lua_pushinteger(L, o.numsides); return true; }
        if (std::strcmp(key, "Text") == 0)         { lua_pushlstring(L, o.text.data(), o.text.size()); return true; }
        if (std::strcmp(key, "Center") == 0 || std::strcmp(key, "Centered") == 0) { lua_pushboolean(L, o.center); return true; }
        if (std::strcmp(key, "Outline") == 0 || std::strcmp(key, "Outlined") == 0) { lua_pushboolean(L, o.outline); return true; }
        if (std::strcmp(key, "OutlineColor") == 0) { push_color3(L, o.outline_color); return true; }
        if (std::strcmp(key, "OutlineOpacity") == 0) { lua_pushnumber(L, o.outline_opacity); return true; }
        if (std::strcmp(key, "Font") == 0)
        {
            if (o.customFont)
            {
                DrawFont::PushFont(L, o.customFont);
                if (!lua_isnil(L, -1))
                    return true;
                lua_pop(L, 1);
            }
            lua_pushinteger(L, static_cast<int>(o.font));
            return true;
        }
        if (o.kind == Kind::Shader && std::strcmp(key, "Vertex") == 0) { lua_pushlstring(L, o.vertex.data(), o.vertex.size()); return true; }
        if (o.kind == Kind::Shader && std::strcmp(key, "Pixel") == 0)  { lua_pushlstring(L, o.pixel.data(), o.pixel.size()); return true; }
        if (std::strcmp(key, "PointA") == 0)       { push_vector2(L, o.pointA); return true; }
        if (std::strcmp(key, "PointB") == 0)       { push_vector2(L, o.pointB); return true; }
        if (std::strcmp(key, "PointC") == 0)       { push_vector2(L, o.pointC); return true; }
        if (std::strcmp(key, "PointD") == 0)       { push_vector2(L, o.pointD); return true; }
        if (std::strcmp(key, "Data") == 0)         { lua_pushlstring(L, o.data.data(), o.data.size()); return true; }
        if (std::strcmp(key, "Rounding") == 0)     { lua_pushnumber(L, o.rounding); return true; }
        if (std::strcmp(key, "Loaded") == 0)       { lua_pushboolean(L, !o.data.empty()); return true; }

        if (std::strcmp(key, "Size") == 0)
        {
            if (o.kind == Kind::Text)
                lua_pushnumber(L, o.textsize);
            else
                push_vector2(L, o.size);
            return true;
        }

        if (std::strcmp(key, "TextBounds") == 0)
        {
            float width = 0.0f;
            float height = 0.0f;
            if (o.customFont && DrawFont::Measure(o.customFont, o.textsize, o.text, width, height))
                push_vector2(L, ImVec2(std::ceil(width), height));
            else if (o.textbounds.x != 0.0f || o.textbounds.y != 0.0f)
                push_vector2(L, o.textbounds);
            else
                push_vector2(L, ImVec2(o.textsize * static_cast<float>(o.text.size()) * 0.5f, o.textsize));
            return true;
        }

        return false;
    }

    inline bool set_property(lua_State* L, Renderer::Object& o, const char* key, int value)
    {
        using Kind = Renderer::Kind;

        if (std::strcmp(key, "Visible") == 0)      { o.visible = lua_toboolean(L, value) != 0; return true; }
        if (std::strcmp(key, "ZIndex") == 0)       { o.zindex = static_cast<int>(lua_tointeger(L, value)); return true; }
        if (std::strcmp(key, "Transparency") == 0) { o.transparency = static_cast<float>(lua_tonumber(L, value)); return true; }
        if (std::strcmp(key, "Color") == 0)        { o.color = to_color3(L, value); return true; }
        if (std::strcmp(key, "Thickness") == 0)    { o.thickness = static_cast<float>(lua_tonumber(L, value)); return true; }
        if (std::strcmp(key, "Filled") == 0)       { o.filled = lua_toboolean(L, value) != 0; return true; }
        if (std::strcmp(key, "From") == 0)         { o.from = to_vector2(L, value); return true; }
        if (std::strcmp(key, "To") == 0)           { o.to = to_vector2(L, value); return true; }
        if (std::strcmp(key, "Position") == 0)     { o.position = to_vector2(L, value); return true; }
        if (std::strcmp(key, "Radius") == 0)       { o.radius = static_cast<float>(lua_tonumber(L, value)); return true; }
        if (std::strcmp(key, "NumSides") == 0)     { o.numsides = static_cast<int>(lua_tointeger(L, value)); return true; }
        if (std::strcmp(key, "Center") == 0 || std::strcmp(key, "Centered") == 0) { o.center = lua_toboolean(L, value) != 0; return true; }
        if (std::strcmp(key, "Outline") == 0 || std::strcmp(key, "Outlined") == 0) { o.outline = lua_toboolean(L, value) != 0; return true; }
        if (std::strcmp(key, "OutlineColor") == 0) { o.outline_color = to_color3(L, value); return true; }
        if (std::strcmp(key, "OutlineOpacity") == 0) { o.outline_opacity = static_cast<float>(lua_tonumber(L, value)); return true; }
        if (std::strcmp(key, "PointA") == 0)       { o.pointA = to_vector2(L, value); return true; }
        if (std::strcmp(key, "PointB") == 0)       { o.pointB = to_vector2(L, value); return true; }
        if (std::strcmp(key, "PointC") == 0)       { o.pointC = to_vector2(L, value); return true; }
        if (std::strcmp(key, "PointD") == 0)       { o.pointD = to_vector2(L, value); return true; }
        if (std::strcmp(key, "Rounding") == 0)     { o.rounding = static_cast<float>(lua_tonumber(L, value)); return true; }

        if (std::strcmp(key, "Text") == 0)
        {
            size_t length = 0;
            const char* text = lua_tolstring(L, value, &length);
            o.text.assign(text ? text : "", text ? length : 0);
            return true;
        }

        if (std::strcmp(key, "Data") == 0)
        {
            size_t length = 0;
            const char* data = lua_tolstring(L, value, &length);
            o.data.assign(data ? data : "", data ? length : 0);

            std::uint64_t hash = 0;
            if (!o.data.empty())
            {
                hash = 0xcbf29ce484222325ULL;
                for (unsigned char c : o.data)
                    hash = (hash ^ c) * 0x100000001b3ULL;
                if (hash == 0)
                    hash = 1;
            }
            o.dataHash = hash;

            if (o.kind == Kind::Font)
                o.customFont = o.data.empty() ? 0 : DrawFont::Create(o.data.data(), o.data.size(), 32.0f, {});
            return true;
        }

        if (o.kind == Kind::Shader && (std::strcmp(key, "Vertex") == 0 || std::strcmp(key, "Pixel") == 0))
        {
            size_t length = 0;
            const char* source = lua_tolstring(L, value, &length);
            (key[0] == 'V' ? o.vertex : o.pixel).assign(source ? source : "", source ? length : 0);
            return true;
        }

        if (std::strcmp(key, "Font") == 0)
        {
            if (const int custom = DrawFont::To(L, value))
            {
                o.customFont = custom;
            }
            else if (Ref* fontRef = to_ref(L, value, false))
            {
                Renderer::Object font;
                if (Renderer::Read(fontRef->handle, font) && font.kind == Kind::Font && font.customFont)
                    o.customFont = font.customFont;
            }
            else if (lua_isnumber(L, value))
            {
                const int font = static_cast<int>(lua_tointeger(L, value));
                if (font >= 0 && font <= 3)
                {
                    o.font = static_cast<Renderer::Font>(font);
                    o.customFont = 0;
                }
            }
            return true;
        }

        if (std::strcmp(key, "Size") == 0)
        {
            if (o.kind == Kind::Text)
                o.textsize = static_cast<float>(lua_tonumber(L, value));
            else
                o.size = to_vector2(L, value);
            return true;
        }

        return false;
    }

    inline bool read_property(lua_State* L, Renderer::Handle handle, const char* key)
    {
        Renderer::Object snapshot;
        if (!Renderer::Read(handle, snapshot))
        {
            auto it = Tombstones.find(handle);
            if (it == Tombstones.end())
                return false;
            snapshot = it->second;
        }

        return get_property(L, snapshot, key);
    }

    inline void write_property(lua_State* L, Renderer::Handle handle, const char* key, int value)
    {
        Renderer::Object staged;
        if (!Renderer::Read(handle, staged))
            return;

        if (set_property(L, staged, key, value))
            Renderer::Write(handle, staged);
    }

    inline int object_destroy_impl(lua_State* L)
    {
        Ref* ref = to_ref(L, 1, false);
        if (!ref)
            return 0;

        Renderer::Object snapshot;
        if (Renderer::Read(ref->handle, snapshot))
        {
            if (snapshot.shaderId)
                WindowOverlay::ReleaseShader(snapshot.shaderId);
            Tombstones[ref->handle] = snapshot;
        }

        Renderer::Destroy(ref->handle);
        return 0;
    }

    inline int object_destroy(lua_State* L)
    {
        unsigned long Fault = 0;
        int res = SafeCall::Invoke(object_destroy_impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
            luaL_error(L, "drawing destroy fault: %s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
            return 0;
        }
        return res;
    }

    inline int shader_create(lua_State* L)
    {
        Ref* ref = to_ref(L, 1, true);
        Renderer::Object object;
        if (!Renderer::Read(ref->handle, object))
            luaL_error(L, "Create: the shader has been destroyed");
        if (object.kind != Renderer::Kind::Shader)
            luaL_error(L, "Create is only valid on Shader drawings");

        std::string error;
        if (!WindowOverlay::CompileShader(object.shaderId, object.vertex, object.pixel, error))
            luaL_error(L, "Create: %s", error.c_str());
        return 0;
    }

    inline int object_index_impl(lua_State* L)
    {
        const char* key = luaL_checkstring(L, 2);

        if (std::strcmp(key, "Destroy") == 0 || std::strcmp(key, "Remove") == 0)
        {
            lua_pushcfunction(L, object_destroy, "Remove");
            return 1;
        }

        if (std::strcmp(key, "Create") == 0)
        {
            lua_pushcfunction(L, shader_create, "Create");
            return 1;
        }

        Ref* ref = to_ref(L, 1, true);

        if (std::strcmp(key, "__OBJECT_EXISTS") == 0)
        {
            lua_pushboolean(L, Renderer::Alive(ref->handle));
            return 1;
        }

        if (std::strcmp(key, "__RENDER_STATE") == 0)
        {
            const std::string state = WindowOverlay::DebugState();
            lua_pushlstring(L, state.data(), state.size());
            return 1;
        }

        if (!read_property(L, ref->handle, key))
            lua_pushnil(L);
        return 1;
    }

    inline int object_index(lua_State* L)
    {
        unsigned long Fault = 0;
        int res = SafeCall::Invoke(object_index_impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
            luaL_error(L, "drawing index fault: %s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
            return 0;
        }
        return res;
    }

    inline int object_newindex_impl(lua_State* L)
    {
        const char* key = luaL_checkstring(L, 2);
        luaL_checkany(L, 3);

        Ref* ref = to_ref(L, 1, true);
        write_property(L, ref->handle, key, 3);
        return 0;
    }

    inline int object_newindex(lua_State* L)
    {
        unsigned long Fault = 0;
        int res = SafeCall::Invoke(object_newindex_impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
            luaL_error(L, "drawing newindex fault: %s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
            return 0;
        }
        return res;
    }

    inline int object_tostring(lua_State* L)
    {
        lua_pushstring(L, "Drawing");
        return 1;
    }

    inline int drawing_new(lua_State* L)
    {
        const char* type = luaL_checkstring(L, 1);

        Renderer::Kind kind;
        if (_stricmp(type, "Line") == 0)                                      kind = Renderer::Kind::Line;
        else if (_stricmp(type, "Text") == 0)                                 kind = Renderer::Kind::Text;
        else if (_stricmp(type, "Circle") == 0)                               kind = Renderer::Kind::Circle;
        else if (_stricmp(type, "Square") == 0 || _stricmp(type, "Rectangle") == 0) kind = Renderer::Kind::Square;
        else if (_stricmp(type, "Quad") == 0)                                 kind = Renderer::Kind::Quad;
        else if (_stricmp(type, "Triangle") == 0)                             kind = Renderer::Kind::Triangle;
        else if (_stricmp(type, "Image") == 0)                                kind = Renderer::Kind::Image;
        else if (_stricmp(type, "Font") == 0)                                 kind = Renderer::Kind::Font;
        else if (_stricmp(type, "Shader") == 0)                               kind = Renderer::Kind::Shader;
        else
        {
            luaL_argerror(L, 1, "invalid Drawing type");
            return 0;
        }

        Renderer::Initialize();
        WindowOverlay::Start();

        Ref* ref = static_cast<Ref*>(lua_newuserdata(L, sizeof(Ref)));
        ref->handle = Renderer::Create(kind);

        if (kind == Renderer::Kind::Shader)
        {
            Renderer::Object object;
            if (Renderer::Read(ref->handle, object))
            {
                object.shaderId = WindowOverlay::NewShader();
                object.visible = true;
                object.size = ImVec2(0.0f, 0.0f);
                Renderer::Write(ref->handle, object);
            }
        }

        luaL_getmetatable(L, MetatableName);
        lua_setmetatable(L, -2);
        return 1;
    }

    inline int isrenderobj(lua_State* L)
    {
        Ref* ref = to_ref(L, 1, false);
        lua_pushboolean(L, ref != nullptr && (Renderer::Alive(ref->handle) || Tombstones.find(ref->handle) != Tombstones.end()));
        return 1;
    }

    inline int getrenderproperty(lua_State* L)
    {
        Ref* ref = to_ref(L, 1, true);
        const char* key = luaL_checkstring(L, 2);

        if (std::strcmp(key, "__OBJECT_EXISTS") == 0)
        {
            lua_pushboolean(L, Renderer::Alive(ref->handle));
            return 1;
        }

        if (!read_property(L, ref->handle, key))
            lua_pushnil(L);
        return 1;
    }

    inline int setrenderproperty(lua_State* L)
    {
        Ref* ref = to_ref(L, 1, true);
        const char* key = luaL_checkstring(L, 2);
        luaL_checkany(L, 3);

        write_property(L, ref->handle, key, 3);
        return 0;
    }

    inline int cleardrawcache(lua_State* L)
    {
        Renderer::Reset();
        Tombstones.clear();
        return 0;
    }

    inline int setcaptureexcluded(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        WindowOverlay::SetCaptureExcluded(lua_toboolean(L, 1) != 0);
        return 0;
    }

    inline int iscaptureexcluded(lua_State* L)
    {
        lua_pushboolean(L, WindowOverlay::IsCaptureExcluded());
        return 1;
    }

    namespace Immediate
    {
        inline char PaintKey;
        inline char DriverKey;

        inline bool g_inPaint = false;
        inline int g_currentZ = 0;
        inline std::vector<Renderer::Object> g_pending;

        inline void require_paint(lua_State* L)
        {
            if (!g_inPaint)
                luaL_error(L, "DrawingImmediate functions can only be called while a paint callback is running");
        }

        inline Renderer::Object make(Renderer::Kind kind, lua_State* L, int color, int opacity)
        {
            Renderer::Object o;
            o.kind = kind;
            o.visible = true;
            o.zindex = g_currentZ;
            o.color = to_color3(L, color);
            o.transparency = static_cast<float>(luaL_checknumber(L, opacity));
            return o;
        }

        inline void push(Renderer::Object o)
        {
            g_pending.push_back(std::move(o));
        }

        inline int Line(lua_State* L)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Line, L, 3, 4);
            o.from = to_vector2(L, 1);
            o.to = to_vector2(L, 2);
            o.thickness = static_cast<float>(luaL_optnumber(L, 5, 1.0));
            push(std::move(o));
            return 0;
        }

        inline int CircleImpl(lua_State* L, bool filled)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Circle, L, 3, 4);
            o.position = to_vector2(L, 1);
            o.radius = static_cast<float>(luaL_checknumber(L, 2));
            o.numsides = static_cast<int>(luaL_optnumber(L, 5, 0));
            o.filled = filled;
            if (!filled)
                o.thickness = static_cast<float>(luaL_optnumber(L, 6, 1.0));
            push(std::move(o));
            return 0;
        }
        inline int Circle(lua_State* L) { return CircleImpl(L, false); }
        inline int FilledCircle(lua_State* L) { return CircleImpl(L, true); }

        inline int TriangleImpl(lua_State* L, bool filled)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Triangle, L, 4, 5);
            o.pointA = to_vector2(L, 1);
            o.pointB = to_vector2(L, 2);
            o.pointC = to_vector2(L, 3);
            o.filled = filled;
            if (!filled)
                o.thickness = static_cast<float>(luaL_optnumber(L, 6, 1.0));
            push(std::move(o));
            return 0;
        }
        inline int Triangle(lua_State* L) { return TriangleImpl(L, false); }
        inline int FilledTriangle(lua_State* L) { return TriangleImpl(L, true); }

        inline int RectangleImpl(lua_State* L, bool filled)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Square, L, 3, 4);
            o.position = to_vector2(L, 1);
            o.size = to_vector2(L, 2);
            o.rounding = static_cast<float>(luaL_optnumber(L, 5, 0.0));
            o.filled = filled;
            if (!filled)
                o.thickness = static_cast<float>(luaL_optnumber(L, 6, 1.0));
            push(std::move(o));
            return 0;
        }
        inline int Rectangle(lua_State* L) { return RectangleImpl(L, false); }
        inline int FilledRectangle(lua_State* L) { return RectangleImpl(L, true); }

        inline int QuadImpl(lua_State* L, bool filled)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Quad, L, 5, 6);
            o.pointA = to_vector2(L, 1);
            o.pointB = to_vector2(L, 2);
            o.pointC = to_vector2(L, 3);
            o.pointD = to_vector2(L, 4);
            o.filled = filled;
            if (!filled)
                o.thickness = static_cast<float>(luaL_optnumber(L, 7, 1.0));
            push(std::move(o));
            return 0;
        }
        inline int Quad(lua_State* L) { return QuadImpl(L, false); }
        inline int FilledQuad(lua_State* L) { return QuadImpl(L, true); }

        inline void set_font(lua_State* L, int index, Renderer::Object& o)
        {
            if (const int custom = DrawFont::To(L, index))
            {
                o.customFont = custom;
                return;
            }
            const int font = static_cast<int>(luaL_checknumber(L, index));
            o.font = static_cast<Renderer::Font>((std::clamp)(font, 0, 3));
        }

        inline int Text(lua_State* L)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Text, L, 4, 5);
            o.position = to_vector2(L, 1);
            set_font(L, 2, o);
            o.textsize = static_cast<float>(luaL_checknumber(L, 3));
            size_t length = 0;
            const char* text = luaL_checklstring(L, 6, &length);
            o.text.assign(text, length);
            o.center = lua_toboolean(L, 7) != 0;
            push(std::move(o));
            return 0;
        }

        inline int OutlinedText(lua_State* L)
        {
            require_paint(L);
            auto o = make(Renderer::Kind::Text, L, 4, 5);
            o.position = to_vector2(L, 1);
            set_font(L, 2, o);
            o.textsize = static_cast<float>(luaL_checknumber(L, 3));
            o.outline = true;
            o.outline_color = to_color3(L, 6);
            o.outline_opacity = static_cast<float>(luaL_checknumber(L, 7));
            size_t length = 0;
            const char* text = luaL_checklstring(L, 8, &length);
            o.text.assign(text, length);
            o.center = lua_toboolean(L, 9) != 0;
            push(std::move(o));
            return 0;
        }

        inline void push_paint_table(lua_State* L)
        {
            lua_pushlightuserdata(L, &PaintKey);
            lua_rawget(L, LUA_REGISTRYINDEX);
        }

        inline int Step(lua_State* L)
        {
            push_paint_table(L);
            const int paints = lua_gettop(L);
            if (!lua_istable(L, paints))
                return 0;

            std::vector<double> layers;
            lua_pushnil(L);
            while (lua_next(L, paints) != 0)
            {
                if (lua_isnumber(L, -2))
                    layers.push_back(lua_tonumber(L, -2));
                lua_pop(L, 1);
            }
            std::sort(layers.begin(), layers.end());

            g_pending.clear();
            g_inPaint = true;
            for (double layer : layers)
            {
                g_currentZ = static_cast<int>(layer);
                lua_pushnumber(L, layer);
                lua_rawget(L, paints);
                LuaSignal::FireFromC(L, lua_gettop(L), 0);
                lua_pop(L, 1);
            }
            g_inPaint = false;

            const bool any = !g_pending.empty();
            Renderer::SetImmediate(L->global, std::move(g_pending));
            g_pending = {};
            if (any)
                WindowOverlay::Start();
            return 0;
        }

        inline void ensure_driver(lua_State* L)
        {
            lua_pushlightuserdata(L, &DriverKey);
            lua_rawget(L, LUA_REGISTRYINDEX);
            const bool connected = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (connected)
                return;

            lua_getglobal(L, "game");
            lua_getfield(L, -1, "GetService");
            lua_pushvalue(L, -2);
            lua_pushstring(L, "RunService");
            lua_call(L, 2, 1);
            lua_getfield(L, -1, "RenderStepped");
            lua_getfield(L, -1, "Connect");
            lua_pushvalue(L, -2);
            Utils::PushGuardedFunction(L, "DrawingImmediate.Step", Step);
            lua_call(L, 2, 1);

            lua_pushlightuserdata(L, &DriverKey);
            lua_insert(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
            lua_pop(L, 3);
        }

        inline int GetPaint(lua_State* L)
        {
            const double z = luaL_optnumber(L, 1, 0);

            push_paint_table(L);
            const int paints = lua_gettop(L);
            lua_pushnumber(L, z);
            lua_rawget(L, paints);
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                LuaSignal::PushNew(L);
                lua_pushnumber(L, z);
                lua_pushvalue(L, -2);
                lua_rawset(L, paints);
            }

            ensure_driver(L);
            return 1;
        }

        inline void RegisterLibrary(lua_State* L)
        {
            lua_pushlightuserdata(L, &PaintKey);
            lua_newtable(L);
            lua_rawset(L, LUA_REGISTRYINDEX);

            lua_newtable(L);
            Utils::AddTableFunction(L, "GetPaint", GetPaint);
            Utils::AddTableFunction(L, "Line", Line);
            Utils::AddTableFunction(L, "Circle", Circle);
            Utils::AddTableFunction(L, "FilledCircle", FilledCircle);
            Utils::AddTableFunction(L, "Triangle", Triangle);
            Utils::AddTableFunction(L, "FilledTriangle", FilledTriangle);
            Utils::AddTableFunction(L, "Rectangle", Rectangle);
            Utils::AddTableFunction(L, "FilledRectangle", FilledRectangle);
            Utils::AddTableFunction(L, "Quad", Quad);
            Utils::AddTableFunction(L, "FilledQuad", FilledQuad);
            Utils::AddTableFunction(L, "Text", Text);
            Utils::AddTableFunction(L, "OutlinedText", OutlinedText);

            lua_getglobal(L, "Drawing");
            lua_getfield(L, -1, "Fonts");
            lua_remove(L, -2);
            lua_setfield(L, -2, "Fonts");

            lua_setreadonly(L, -1, true);
            lua_setglobal(L, "DrawingImmediate");
        }
    }

    inline void RegisterLibrary(lua_State* L)
    {
        ResetLibraryState();

        luaL_newmetatable(L, MetatableName);

        lua_pushcfunction(L, object_index, "__index");
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, object_newindex, "__newindex");
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, object_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");

        lua_pushstring(L, "Drawing");
        lua_setfield(L, -2, "__type");

        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");

        lua_pop(L, 1);

        lua_newtable(L);
        Utils::AddTableFunction(L, "new", drawing_new);
        Utils::AddTableFunction(L, "New", drawing_new);
        Utils::AddTableFunction(L, "setcaptureexcluded", setcaptureexcluded);
        Utils::AddTableFunction(L, "iscaptureexcluded", iscaptureexcluded);

        lua_newtable(L);
        lua_pushinteger(L, 0); lua_setfield(L, -2, "UI");
        lua_pushinteger(L, 1); lua_setfield(L, -2, "System");
        lua_pushinteger(L, 2); lua_setfield(L, -2, "Plex");
        lua_pushinteger(L, 3); lua_setfield(L, -2, "Monospace");
        lua_setreadonly(L, -1, true);
        lua_setfield(L, -2, "Fonts");

        lua_setglobal(L, "Drawing");

        Utils::AddFunction(L, "isrenderobj", isrenderobj);
        Utils::AddFunction(L, "isrenderobject", isrenderobj);
        Utils::AddFunction(L, "is_render_obj", isrenderobj);
        Utils::AddFunction(L, "getrenderproperty", getrenderproperty);
        Utils::AddFunction(L, "get_render_property", getrenderproperty);
        Utils::AddFunction(L, "setrenderproperty", setrenderproperty);
        Utils::AddFunction(L, "set_render_property", setrenderproperty);
        Utils::AddFunction(L, "cleardrawcache", cleardrawcache);
        Utils::AddFunction(L, "clear_draw_cache", cleardrawcache);
        Utils::AddFunction(L, "setcaptureexcluded", setcaptureexcluded);
        Utils::AddFunction(L, "set_capture_excluded", setcaptureexcluded);
        Utils::AddFunction(L, "iscaptureexcluded", iscaptureexcluded);
        Utils::AddFunction(L, "is_capture_excluded", iscaptureexcluded);

        DrawFont::RegisterLibrary(L);
        Immediate::RegisterLibrary(L);
    }
}
