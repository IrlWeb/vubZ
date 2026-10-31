#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Render
{
    enum class Kind
    {
        Line,
        Text,
        Image,
        Circle,
        Square,
        Quad,
        Triangle,
    };

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Color
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
    };

    using Handle = std::uint32_t;
    constexpr Handle InvalidHandle = 0;

    struct Object
    {
        Kind kind = Kind::Line;

        bool visible = false;
        bool filled = false;
        bool center = false;
        bool outline = false;

        int zindex = 0;
        int numsides = 0;
        int font = 0;

        float transparency = 1.0f;
        float thickness = 1.0f;
        float radius = 0.0f;
        float textsize = 13.0f;

        Color color{};
        Color outlinecolor{};

        Vec2 from{};
        Vec2 to{};
        Vec2 position{};
        Vec2 size{};
        Vec2 textbounds{};

        Vec2 pointA{};
        Vec2 pointB{};
        Vec2 pointC{};
        Vec2 pointD{};

        std::string text;
        std::string imagedata;
        std::uint32_t imageversion = 0;
    };

    std::mutex& Lock();

    Handle Create(Kind kind);

    Object* Get(Handle handle);

    bool Alive(Handle handle);
    void Destroy(Handle handle);
    void Reset();

    bool Initialize();
    void Shutdown();
}
