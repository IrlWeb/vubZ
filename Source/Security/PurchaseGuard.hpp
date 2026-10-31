#pragma once

#include <Core/Context.hpp>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Coroutines/Yielding.hpp>
#include <Ui/Overlay/WindowOverlay.hpp>

#include <ShlObj.h>
#include <lapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace PurchaseGuard
{
    struct Method
    {
        const char* name;
        const char* kind;
        int idArg;
        const char* infoType;
        int priceArg;
    };

    inline constexpr Method Methods[] = {
        { "PromptPurchase",                      "Catalog asset",       3, "Asset",        0 },
        { "PromptProductPurchase",               "Developer product",   3, "Product",      0 },
        { "PromptGamePassPurchase",              "Game pass",           3, "GamePass",     0 },
        { "PromptBundlePurchase",                "Bundle",              3, "Bundle",       0 },
        { "PromptSubscriptionPurchase",          "Subscription",        3, "Subscription", 0 },
        { "PromptCancelSubscription",            "Cancel subscription", 3, "Subscription", 0 },
        { "PromptPremiumPurchase",               "Roblox Premium",      0, nullptr,        0 },
        { "PromptBulkPurchase",                  "Bulk purchase",       0, nullptr,        0 },
        { "PromptRobloxPurchase",                "Catalog asset",       2, "Asset",        0 },
        { "PromptThirdPartyPurchase",            "Third-party product", 3, nullptr,        0 },
        { "PromptNativePurchase",                "Platform product",    3, nullptr,        0 },
        { "PromptNativePurchaseWithLocalPlayer", "Platform product",    2, nullptr,        0 },
        { "PromptCollectiblesPurchase",          "Limited collectible", 3, "Asset",        7 },
    };

    inline constexpr unsigned long long DecisionTimeoutMs = 60 * 1000;
    inline constexpr unsigned long long CallerTimeoutMs = 3 * 60 * 1000;

    struct Pending
    {
        int method = 0;
        std::string id;
        std::string priceHint;
        std::atomic<bool> asked{ false };
        std::atomic<bool> allowed{ false };
        std::atomic<bool> invoked{ false };
        std::atomic<bool> finished{ false };
    };

    inline std::mutex g_lock;
    inline std::unordered_map<int, std::shared_ptr<Pending>> g_pending;
    inline int g_nextToken = 1;

    inline thread_local lua_State* t_bypassThread = nullptr;
    inline thread_local int t_bypassMethod = -1;

    inline int Find(const char* name)
    {
        if (!name || name[0] != 'P')
            return -1;
        for (int i = 0; i < static_cast<int>(std::size(Methods)); ++i)
            if (std::strcmp(name, Methods[i].name) == 0)
                return i;
        return -1;
    }

    inline bool ConsumeBypass(lua_State* L, int method)
    {
        if (t_bypassThread != L || t_bypassMethod != method)
            return false;
        t_bypassThread = nullptr;
        t_bypassMethod = -1;
        return true;
    }

    inline bool Enabled()
    {
        static std::atomic<unsigned long long> checkedAt{ 0 };
        static std::atomic<bool> enabled{ false };

        const unsigned long long now = GetTickCount64();
        const unsigned long long last = checkedAt.load();
        if (last != 0 && now - last < 2000)
            return enabled.load();
        checkedAt.store(now);

        bool on = false;
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        {
            wchar_t path[MAX_PATH];
            on = swprintf_s(path, L"%s\\supQ\\purchases.on", appData) > 0
                && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
            CoTaskMemFree(appData);
        }
        enabled.store(on);
        return on;
    }

    inline std::string Describe(lua_State* L, int index)
    {
        if (index <= 0 || index > lua_gettop(L))
            return {};

        char buffer[64];
        switch (lua_type(L, index))
        {
        case LUA_TNUMBER:
        {
            const double value = lua_tonumber(L, index);
            if (value == static_cast<double>(static_cast<long long>(value)))
                _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%lld", static_cast<long long>(value));
            else
                _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%g", value);
            return buffer;
        }
        case LUA_TSTRING:
        {
            size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            return std::string(text, (std::min)(length, static_cast<size_t>(96)));
        }
        default:
            return {};
        }
    }

    inline std::shared_ptr<Pending> Lookup(lua_State* L)
    {
        const int token = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_pending.find(token);
        return it == g_pending.end() ? nullptr : it->second;
    }

    inline int Ask(lua_State* L)
    {
        const std::shared_ptr<Pending> pending = Lookup(L);
        if (!pending || pending->asked.exchange(true))
        {
            lua_pushboolean(L, false);
            return 1;
        }

        const Method& method = Methods[pending->method];
        auto prompt = std::make_shared<Renderer::Gui::PurchasePrompt>();
        prompt->method = method.name;
        prompt->kind = method.kind;
        prompt->id = pending->id;
        if (lua_type(L, 1) == LUA_TSTRING)
            prompt->item = lua_tostring(L, 1);
        else
            prompt->item = method.infoType ? "Unknown (product info lookup failed)" : "No product info for this prompt type";
        if (lua_type(L, 2) == LUA_TNUMBER)
            prompt->price = "R$ " + Describe(L, 2);
        else if (!pending->priceHint.empty())
            prompt->price = "R$ " + pending->priceHint + " (as passed by the script)";
        else
            prompt->price = "Unknown (Roblox's own prompt shows it)";

        WindowOverlay::Start();
        Roblox::Print(1, "MarketplaceService:%s is waiting for Allow / Deny in the in-game purchase dialog", method.name);

        return Yielding::YieldExecution(L, [pending, prompt]() -> Yielded
        {
            prompt->deadline = Renderer::Gui::NowMs() + DecisionTimeoutMs;
            Renderer::Gui::PushPrompt(prompt);
            while (prompt->decision.load() == 0 && Renderer::Gui::NowMs() < prompt->deadline + 1000)
                Sleep(50);
            Renderer::Gui::ClosePrompt(prompt, 2);

            const bool allowed = prompt->decision.load() == 1;
            pending->allowed.store(allowed);
            return [allowed](lua_State* L) -> int
            {
                lua_pushboolean(L, allowed);
                return 1;
            };
        });
    }

    inline int Invoke(lua_State* L)
    {
        const std::shared_ptr<Pending> pending = Lookup(L);
        if (!pending || !pending->allowed.load() || pending->invoked.exchange(true))
            return 0;

        const int nargs = lua_gettop(L);
        if (nargs < 1 || lua_type(L, 1) != LUA_TUSERDATA)
            return 0;

        const char* name = Methods[pending->method].name;
        t_bypassThread = L;
        t_bypassMethod = pending->method;
        lua_getfield(L, 1, name);
        t_bypassThread = nullptr;
        t_bypassMethod = -1;

        lua_insert(L, 1);
        if (lua_pcall(L, nargs, 0, 0) != LUA_OK)
        {
            Roblox::Print(2, "MarketplaceService:%s failed: %s", name, lua_isstring(L, -1) ? lua_tostring(L, -1) : "error");
            lua_pop(L, 1);
        }
        return 0;
    }

    inline int Finish(lua_State* L)
    {
        if (const std::shared_ptr<Pending> pending = Lookup(L))
            pending->finished.store(true);
        return 0;
    }

    inline constexpr const char* GateSource = R"(local ask, invoke, finish, infoType, id = ...
local args = table.pack(select(6, ...))
pcall(function()
    local name, price
    if infoType ~= nil and id ~= nil then
        local ok, info = pcall(function()
            return game:GetService("MarketplaceService"):GetProductInfo(id, Enum.InfoType[infoType])
        end)
        if ok and type(info) == "table" then
            name = info.Name or info.DisplayName
            price = info.PriceInRobux
        end
    end
    if ask(name, price) then
        invoke(table.unpack(args, 1, args.n))
    end
end)
finish()
)";

    inline void PushControl(lua_State* T, lua_CFunction fn, int token)
    {
        lua_pushinteger(T, token);
        lua_pushcclosure(T, fn, nullptr, 1);
    }

    inline int Intercept(lua_State* L, int methodIndex)
    {
        const Method& method = Methods[methodIndex];
        if (!Enabled())
        {
            luaL_error(L, "Function '%s' has been blocked for security reasons", method.name);
            return 0;
        }
        if (!lua_isyieldable(L))
        {
            luaL_error(L, "%s needs a yieldable thread to ask for confirmation", method.name);
            return 0;
        }

        static const std::string Bytecode = Execution::PrepareScript(GateSource);
        if (Bytecode.empty())
        {
            luaL_error(L, "%s: confirmation dialog unavailable", method.name);
            return 0;
        }

        auto pending = std::make_shared<Pending>();
        pending->method = methodIndex;
        pending->id = Describe(L, method.idArg);
        pending->priceHint = Describe(L, method.priceArg);
        if (methodIndex == Find("PromptBulkPurchase") && lua_istable(L, 3))
            pending->id = std::to_string(lua_objlen(L, 3)) + " item(s)";

        int token = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (!g_pending.empty())
            {
                Roblox::Print(2, "MarketplaceService:%s dropped: another purchase prompt is waiting for an answer", method.name);
                return 0;
            }
            token = g_nextToken++;
            g_pending[token] = pending;
        }
        const auto drop = [token]()
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_pending.erase(token);
        };

        const int nargs = lua_gettop(L);
        const int top = nargs;
        lua_State* T = lua_newthread(L);
        luaL_sandboxthread(T);
        if (SharedVariables::ExecutorFastcallsDisabled)
            lua_setsafeenv(T, LUA_GLOBALSINDEX, false);
        TaskScheduler::SetThreadCapabilities(T, 8, MaxCapabilities);

        if (luau_load(T, "=purchase", Bytecode.data(), Bytecode.size(), 0) != LUA_OK)
        {
            lua_settop(L, top);
            drop();
            luaL_error(L, "%s: confirmation dialog unavailable", method.name);
            return 0;
        }
        TaskScheduler::SetProtoCapabilities(clvalue(luaA_toobject(T, -1))->l.p, &MaxCapabilities);

        lua_getglobal(T, "task");
        lua_getfield(T, -1, "defer");
        lua_remove(T, -2);
        lua_insert(T, -2);

        lua_rawcheckstack(T, nargs + 6);
        PushControl(T, Ask, token);
        PushControl(T, Invoke, token);
        PushControl(T, Finish, token);
        if (method.infoType)
            lua_pushstring(T, method.infoType);
        else
            lua_pushnil(T);
        if (method.idArg > 0 && method.idArg <= nargs)
            lua_xpush(L, T, method.idArg);
        else
            lua_pushnil(T);
        for (int i = 1; i <= nargs; ++i)
            lua_xpush(L, T, i);

        if (lua_pcall(T, 1 + 5 + nargs, 0, 0) != LUA_OK)
        {
            lua_settop(L, top);
            drop();
            luaL_error(L, "%s: confirmation dialog unavailable", method.name);
            return 0;
        }
        lua_settop(L, top);

        return Yielding::YieldExecution(L, [pending, token]() -> Yielded
        {
            const unsigned long long deadline = GetTickCount64() + CallerTimeoutMs;
            while (!pending->finished.load() && GetTickCount64() < deadline)
                Sleep(50);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_pending.erase(token);
            }
            return [](lua_State*) -> int { return 0; };
        });
    }

    inline int Wrapper(lua_State* L)
    {
        return Intercept(L, static_cast<int>(lua_tointeger(L, lua_upvalueindex(1))));
    }

    inline int PushWrapper(lua_State* L, int methodIndex)
    {
        if (!Enabled())
        {
            luaL_error(L, "Function '%s' has been blocked for security reasons", Methods[methodIndex].name);
            return 0;
        }
        lua_pushinteger(L, methodIndex);
        lua_pushcclosure(L, Wrapper, Methods[methodIndex].name, 1);
        return 1;
    }
}
