#pragma once

#include <Core/DebugLog.hpp>
#include <lstate.h>
#include <lualib.h>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include "Update/Offsets.hpp"
#include <Windows.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <type_traits>
#include <cstring>

namespace Reflection
{
    struct CallbackRecord
    {
        int FunctionRef = LUA_NOREF;
        uint64_t Generation = 0;
    };

    struct ScriptableOverride
    {
        uintptr_t Descriptor = 0;
        bool Scriptable = false;
    };

    inline std::unordered_map<uintptr_t, std::unordered_map<std::string, ScriptableOverride>> scriptable_overrides;
    inline std::unordered_map<uintptr_t, std::unordered_map<std::string, CallbackRecord>> callback_values;

    inline bool can_read(uintptr_t address, size_t size = sizeof(uintptr_t)) noexcept
    {
        if (address < 0x10000)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;

        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return address + size >= address && address + size <= regionEnd;
    }

    inline uintptr_t seh_read_ptr(uintptr_t addr) noexcept {
        __try { return *reinterpret_cast<uintptr_t*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    inline bool seh_write_u32(uintptr_t addr, uint32_t val) noexcept {
        __try { *reinterpret_cast<uint32_t*>(addr) = val; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline bool seh_try_read_u32(uintptr_t addr, uint32_t& out) noexcept {
        __try { out = *reinterpret_cast<uint32_t*>(addr); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline uint32_t seh_read_u32(uintptr_t addr) noexcept {
        __try { return *reinterpret_cast<uint32_t*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    template<typename T>
    static bool read_value(std::uintptr_t address, T& result)
    {
        static_assert(std::is_trivially_copyable_v<T>);

        if (!address || !can_read(address, sizeof(T)))
            return false;

        std::memcpy(&result, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    namespace Checker {
        static bool CheckPointer(uintptr_t Address) {
            __try {
                if (Address < 0x10000) {
                    return false;
                }

                MEMORY_BASIC_INFORMATION MemoryInfo = {};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(Address), &MemoryInfo, sizeof(MemoryInfo)) == 0) {
                    return false;
                }

                if (MemoryInfo.State != MEM_COMMIT) {
                    return false;
                }

                if (MemoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) {
                    return false;
                }

                return (MemoryInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    inline const char* read_reflection_string(uintptr_t stringObject)
    {
        if (!stringObject || !can_read(stringObject, 0x20))
            return nullptr;

        size_t length = 0;
        size_t capacity = 0;
        if (!read_value(stringObject + 0x10, length) || !read_value(stringObject + 0x18, capacity))
            return nullptr;

        if (length == 0 || length > 512 || capacity < length)
            return nullptr;

        uintptr_t data = stringObject;
        if (capacity >= 0x10)
        {
            data = seh_read_ptr(stringObject);
            if (!data)
                return nullptr;
        }

        if (!can_read(data, length + 1))
            return nullptr;

        const char* text = reinterpret_cast<const char*>(data);
        if (text[length] != '\0' || text[0] < 0x20 || text[0] > 0x7E)
            return nullptr;

        return text;
    }

    inline const char* get_property_name(uintptr_t prop)
    {
        if (!prop) return nullptr;

        if (const char* name = read_reflection_string(seh_read_ptr(prop + Offsets::PropertyDescriptor::Name)))
            return name;

        return read_reflection_string(prop + Offsets::PropertyDescriptor::Name);
    }

    inline uintptr_t get_instance_prop_by_name(lua_State* L, uintptr_t instance, const char* propName)
    {
        if (!instance || !propName || !can_read(instance + Offsets::Instance::ClassDescriptor, sizeof(uintptr_t)))
            return 0;

        uintptr_t classDescriptor = seh_read_ptr(instance + Offsets::Instance::ClassDescriptor);
        if (!classDescriptor)
            return 0;

        uintptr_t super = classDescriptor;
        std::unordered_set<uintptr_t> visited;

        while (super && visited.find(super) == visited.end())
        {
            visited.insert(super);

            if (can_read(super + 0x28, sizeof(uintptr_t) * 2))
            {
                uintptr_t start = seh_read_ptr(super + 0x28);
                uintptr_t end = seh_read_ptr(super + 0x30);

                if (start && end && end >= start && (end - start) <= 4096 * sizeof(uintptr_t))
                {
                    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t))
                    {
                        uintptr_t prop = seh_read_ptr(cur);
                        if (!prop) continue;

                        const char* name = get_property_name(prop);
                        if (name && _stricmp(name, propName) == 0)
                            return prop;
                    }
                }
                else if (start)
                {
                    for (uint32_t i = 0; i < 512; ++i)
                    {
                        uintptr_t entry = start + (i * 0x10);
                        if (!can_read(entry, sizeof(uintptr_t)))
                            break;

                        uintptr_t prop = seh_read_ptr(entry);
                        if (!prop) continue;

                        const char* name = get_property_name(prop);
                        if (name && _stricmp(name, propName) == 0)
                            return prop;
                    }
                }
            }

            if (can_read(super + Offsets::Instance::Properties, sizeof(uintptr_t) * 2))
            {
                uintptr_t start = seh_read_ptr(super + Offsets::Instance::Properties);
                uintptr_t end = seh_read_ptr(super + Offsets::Instance::Properties + 0x8);

                if (start && end && end >= start && (end - start) <= 4096 * sizeof(uintptr_t))
                {
                    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t))
                    {
                        uintptr_t prop = seh_read_ptr(cur);
                        if (!prop) continue;

                        const char* name = get_property_name(prop);
                        if (name && _stricmp(name, propName) == 0)
                            return prop;
                    }
                }
            }

            if (!can_read(super + 0x8, sizeof(uintptr_t)))
                break;
            uintptr_t parent = seh_read_ptr(super + 0x8);
            if (!parent || parent == super)
                break;
            super = parent;
        }

        return 0;
    }

    constexpr std::uintptr_t ClassPropertyEntriesOffset = 0x1E8;
    constexpr std::uintptr_t ClassPropertyMaskOffset = 0x1F0;
    constexpr std::uintptr_t ClassPropertyEntrySize = 0x10;

    inline void collect_class_properties(std::uintptr_t classDescriptor, std::vector<uintptr_t>& props)
    {
        std::uintptr_t entries = 0;
        std::uint32_t mask = 0;
        if (!read_value(classDescriptor + ClassPropertyEntriesOffset, entries) || !entries)
            return;
        if (!read_value(classDescriptor + ClassPropertyMaskOffset, mask) || mask == 0xFFFFFFFFu)
            return;

        const std::uint32_t slots = mask + 1;
        if (slots > 8192)
            return;

        for (std::uint32_t i = 0; i < slots; ++i)
        {
            std::uintptr_t descriptor = 0;
            if (!read_value(entries + static_cast<std::uintptr_t>(i) * ClassPropertyEntrySize, descriptor) || !descriptor)
                continue;
            if (can_read(descriptor + Offsets::Instance::PropertyDescriptorBitFlags, sizeof(uint32_t)) &&
                get_property_name(descriptor))
                props.push_back(descriptor);
        }
    }

    inline std::vector<uintptr_t> get_all_properties_for_instance(uintptr_t instance)
    {
        std::vector<uintptr_t> props;

        if (!instance || !can_read(instance + Offsets::Instance::ClassDescriptor, sizeof(uintptr_t)))
            return props;

        uintptr_t classDescriptor = seh_read_ptr(instance + Offsets::Instance::ClassDescriptor);
        if (!classDescriptor)
            return props;

        collect_class_properties(classDescriptor, props);
        if (!props.empty())
            return props;

        uintptr_t super = classDescriptor;
        std::unordered_set<uintptr_t> visited;

        while (super && visited.find(super) == visited.end())
        {
            visited.insert(super);

            if (can_read(super + 0x28, sizeof(uintptr_t) * 2))
            {
                uintptr_t start = seh_read_ptr(super + 0x28);
                uintptr_t end = seh_read_ptr(super + 0x30);

                if (start && end && end >= start && (end - start) <= 4096 * sizeof(uintptr_t))
                {
                    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t))
                    {
                        uintptr_t prop = seh_read_ptr(cur);
                        if (prop && can_read(prop + Offsets::Instance::PropertyDescriptorBitFlags, sizeof(uint32_t)))
                        {
                            props.push_back(prop);
                        }
                    }
                }
                else if (start)
                {
                    for (uint32_t i = 0; i < 512; ++i)
                    {
                        uintptr_t entry = start + (i * 0x10);
                        if (!can_read(entry, sizeof(uintptr_t)))
                            break;

                        uintptr_t prop = seh_read_ptr(entry);
                        if (prop && can_read(prop + Offsets::Instance::PropertyDescriptorBitFlags, sizeof(uint32_t)))
                        {
                            props.push_back(prop);
                        }
                    }
                }
            }

            if (can_read(super + Offsets::Instance::Properties, sizeof(uintptr_t) * 2))
            {
                uintptr_t start = seh_read_ptr(super + Offsets::Instance::Properties);
                uintptr_t end = seh_read_ptr(super + Offsets::Instance::Properties + 0x8);

                if (start && end && end >= start && (end - start) <= 4096 * sizeof(uintptr_t))
                {
                    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t))
                    {
                        uintptr_t prop = seh_read_ptr(cur);
                        if (prop && can_read(prop + Offsets::Instance::PropertyDescriptorBitFlags, sizeof(uint32_t)))
                        {
                            props.push_back(prop);
                        }
                    }
                }
            }

            if (!can_read(super + 0x8, sizeof(uintptr_t)))
                break;
            uintptr_t parent = seh_read_ptr(super + 0x8);
            if (!parent || parent == super)
                break;
            super = parent;
        }

        return props;
    }

    inline void ResetLibraryState()
    {
        scriptable_overrides.clear();
        callback_values.clear();
    }

    inline void TrackCallbackValue(lua_State* L, int instanceIndex, int propertyIndex, int valueIndex)
    {
        if (!L || !lua_isuserdata(L, instanceIndex) || !lua_isstring(L, propertyIndex) ||
            !lua_isfunction(L, valueIndex))
            return;

        uintptr_t instance = 0;
        void* userdata = lua_touserdata(L, instanceIndex);
        if (!userdata || !can_read(reinterpret_cast<uintptr_t>(userdata)) ||
            !(instance = *reinterpret_cast<uintptr_t*>(userdata)))
            return;

        const char* property = lua_tostring(L, propertyIndex);
        if (!property || !property[0])
            return;

        lua_pushvalue(L, valueIndex);
        const int functionRef = lua_ref(L, -1);
        lua_pop(L, 1);

        auto& record = callback_values[instance][property];

        if (record.FunctionRef != LUA_NOREF &&
            record.Generation == SharedVariables::StateGeneration.load())
            lua_unref(L, record.FunctionRef);

        record.FunctionRef = functionRef;
        record.Generation = SharedVariables::StateGeneration.load();
    }

    inline bool seh_string_equals(uintptr_t stringObject, const char* expected, size_t expectedLength) noexcept
    {
        __try
        {
            const size_t length = *reinterpret_cast<const size_t*>(stringObject + 0x10);
            const size_t capacity = *reinterpret_cast<const size_t*>(stringObject + 0x18);
            if (length != expectedLength || capacity < length)
                return false;
            const char* data = capacity >= 0x10 ? *reinterpret_cast<const char* const*>(stringObject) : reinterpret_cast<const char*>(stringObject);
            return data && std::memcmp(data, expected, length) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool seh_property_name_equals(uintptr_t prop, const char* expected, size_t expectedLength) noexcept
    {
        const uintptr_t holder = seh_read_ptr(prop + Offsets::PropertyDescriptor::Name);
        if (holder >= 0x10000 && holder < 0x7FFFFFFF0000 && (holder & 7) == 0 &&
            seh_string_equals(holder, expected, expectedLength))
            return true;
        return seh_string_equals(prop + Offsets::PropertyDescriptor::Name, expected, expectedLength);
    }

    inline std::uintptr_t find_property_in_class(std::uintptr_t classDescriptor, const char* propertyName)
    {
        constexpr std::uintptr_t EntriesOffset = ClassPropertyEntriesOffset;
        constexpr std::uintptr_t MaskOffset = ClassPropertyMaskOffset;
        constexpr std::uintptr_t EntrySize = ClassPropertyEntrySize;

        if (!classDescriptor || !propertyName)
            return 0;

        std::uintptr_t entries = 0;
        std::uint32_t mask = 0;
        if (!read_value(classDescriptor + EntriesOffset, entries) || !entries)
            return 0;
        if (!read_value(classDescriptor + MaskOffset, mask) || mask == 0xFFFFFFFFu)
            return 0;

        const std::uint32_t slots = mask + 1;
        if (slots > 8192 || !can_read(entries, static_cast<size_t>(slots) * EntrySize))
            return 0;

        const size_t nameLength = std::strlen(propertyName);
        for (std::uint32_t i = 0; i < slots; ++i)
        {
            const std::uintptr_t descriptor = *reinterpret_cast<const std::uintptr_t*>(entries + static_cast<std::uintptr_t>(i) * EntrySize);
            if (descriptor && seh_property_name_equals(descriptor, propertyName, nameLength))
                return descriptor;
        }

        return 0;
    }

    inline std::mutex property_cache_mutex;
    inline std::unordered_map<std::uintptr_t, std::unordered_map<std::string, std::uintptr_t>> property_cache;

    inline bool resolve_property(lua_State* L, std::uintptr_t& out_instance, std::uintptr_t& out_prop)
    {
        constexpr std::uintptr_t ClassDescriptorOffset = 0x18;

        out_instance = 0;
        out_prop = 0;

        if (!lua_isuserdata(L, 1))
            return false;

        void* userdata = lua_touserdata(L, 1);
        if (!userdata || lua_objlen(L, 1) < sizeof(std::uintptr_t))
            return false;

        out_instance = *reinterpret_cast<std::uintptr_t*>(userdata);
        if (out_instance < 0x10000)
            return false;

        const char* propertyName = lua_tostring(L, 2);
        if (!propertyName || propertyName[0] == '\0')
            return false;

        const std::uintptr_t classDescriptor = seh_read_ptr(out_instance + ClassDescriptorOffset);

        if (classDescriptor)
        {
            std::lock_guard<std::mutex> lock(property_cache_mutex);
            const auto cls = property_cache.find(classDescriptor);
            if (cls != property_cache.end())
            {
                const auto hit = cls->second.find(propertyName);
                if (hit != cls->second.end())
                {
                    out_prop = hit->second;
                    return out_prop != 0;
                }
            }
        }

        std::uintptr_t descriptor = classDescriptor ? find_property_in_class(classDescriptor, propertyName) : 0;
        if (descriptor && !can_read(descriptor + Offsets::Instance::PropertyDescriptorBitFlags, sizeof(uint32_t)))
            descriptor = 0;
        if (!descriptor)
            descriptor = get_instance_prop_by_name(L, out_instance, propertyName);

        if (classDescriptor)
        {
            std::lock_guard<std::mutex> lock(property_cache_mutex);
            property_cache[classDescriptor][propertyName] = descriptor;
        }

        out_prop = descriptor;
        return (out_prop != 0);
    }

    inline bool read_scriptable_flag(uintptr_t prop) noexcept
    {
        if (!prop)
            return false;

        return (seh_read_u32(prop + Offsets::Instance::PropertyDescriptorBitFlags) & 0x10) != 0;
    }

    inline void write_scriptable_flag(uintptr_t prop, bool state) noexcept
    {
        if (!prop)
            return;

        uint32_t flags = seh_read_u32(prop + Offsets::Instance::PropertyDescriptorBitFlags);
        seh_write_u32(prop + Offsets::Instance::PropertyDescriptorBitFlags, (flags & ~0x10u) | (state ? 0x10u : 0));
    }

    struct PropertyAccess
    {
        int Type = -1;
        uintptr_t Getter = 0;
        uintptr_t GetSet = 0;
    };

    inline std::unordered_map<uintptr_t, PropertyAccess> property_access_cache;

    inline int read_property_type(uintptr_t prop) {
        uintptr_t ttype = seh_read_ptr(prop + Offsets::PropertyDescriptor::TypeInfo);
        if (!ttype || !Checker::CheckPointer(ttype))
            return -1;
        return *reinterpret_cast<int*>(ttype + 0x30);
    }

    inline uintptr_t get_property_getter(uintptr_t prop);

    inline PropertyAccess get_property_access(uintptr_t prop) {
        {
            std::lock_guard<std::mutex> lock(property_cache_mutex);
            const auto hit = property_access_cache.find(prop);
            if (hit != property_access_cache.end())
                return hit->second;
        }

        PropertyAccess access;
        access.Type = read_property_type(prop);
        access.Getter = get_property_getter(prop);
        const uintptr_t getset = seh_read_ptr(prop + Offsets::PropertyDescriptor::GetSet);
        if (getset && Checker::CheckPointer(getset))
            access.GetSet = getset;

        std::lock_guard<std::mutex> lock(property_cache_mutex);
        property_access_cache[prop] = access;
        return access;
    }

    inline int get_property_type(uintptr_t prop) {
        return get_property_access(prop).Type;
    }

    inline uintptr_t get_property_getter(uintptr_t prop) {
        uintptr_t get_set_impl = seh_read_ptr(prop + Offsets::PropertyDescriptor::GetSet);
        if (!get_set_impl || !Checker::CheckPointer(get_set_impl))
            return 0;

        uintptr_t get_set_vft = seh_read_ptr(get_set_impl);
        if (!get_set_vft || !Checker::CheckPointer(get_set_vft))
            return 0;

        return seh_read_ptr(get_set_vft + 0x18);
    }

    inline uintptr_t get_property_setter(uintptr_t prop) {
        uintptr_t get_set_impl = seh_read_ptr(prop + Offsets::PropertyDescriptor::GetSet);
        if (!get_set_impl || !Checker::CheckPointer(get_set_impl))
            return 0;

        uintptr_t get_set_vft = seh_read_ptr(get_set_impl);
        if (!get_set_vft || !Checker::CheckPointer(get_set_vft))
            return 0;

        return seh_read_ptr(get_set_vft + 0x20);
    }

    inline int protected_getfield_impl(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* property = luaL_checkstring(L, 2);
        lua_getfield(L, 1, property);
        return 1;
    }

    inline int protected_getfield(lua_State* L)
    {
        unsigned long Fault = 0;
        int res = SafeCall::Invoke(protected_getfield_impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
            luaL_error(L, "getfield fault: %s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
            return 0;
        }
        return res;
    }

    inline int protected_setfield_impl(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* property = luaL_checkstring(L, 2);
        lua_pushvalue(L, 3);
        lua_setfield(L, 1, property);
        return 0;
    }

    inline int protected_setfield(lua_State* L)
    {
        unsigned long Fault = 0;
        int res = SafeCall::Invoke(protected_setfield_impl, L, &Fault);
        if (Fault != 0)
        {
            if (Fault == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
            luaL_error(L, "setfield fault: %s (0x%08X)", SafeCall::FaultName(Fault), static_cast<unsigned int>(Fault));
            return 0;
        }
        return res;
    }

    inline bool set_property_value(lua_State* L, int instanceIndex, int propertyIndex, int valueIndex)
    {
        instanceIndex = lua_absindex(L, instanceIndex);
        propertyIndex = lua_absindex(L, propertyIndex);
        valueIndex = lua_absindex(L, valueIndex);
        const int top = lua_gettop(L);

        lua_pushcfunction(L, protected_setfield, "sethiddenproperty_set");
        lua_pushvalue(L, instanceIndex);
        lua_pushvalue(L, propertyIndex);
        lua_pushvalue(L, valueIndex);

        if (lua_pcall(L, 3, 0, 0) != LUA_OK)
        {
            lua_settop(L, top);
            return false;
        }

        lua_settop(L, top);
        return true;
    }

    inline bool call_string_getter_seh(uintptr_t getter_addr, void* getset, void* inst, std::string* output)
    {
        using Fn = void(__fastcall*)(void*, void*, void*);
        __try
        {
            reinterpret_cast<Fn>(getter_addr)(getset, output, inst);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    inline bool call_content_getter_seh(uintptr_t getterAddress, void* getset, void* instance, std::string& out)
    {
        using GetterFn = void(__fastcall*)(void*, uintptr_t*, void*);

        out.clear();
        uintptr_t sharedString = 0;

        __try
        {
            reinterpret_cast<GetterFn>(getterAddress)(getset, &sharedString, instance);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        if (!sharedString || !Checker::CheckPointer(sharedString))
            return false;

        // 744 interned SharedString entry (sub_144AA9050): 16-byte hash key, std::string at +0x10
        // (buffer/ptr +0x10, size +0x20, capacity +0x28), id +0x30, refcount +0x38.
        constexpr uintptr_t StringOffset = 0x10;
        constexpr size_t MaximumLength = 64ull * 1024ull * 1024ull;
        size_t length = 0;
        size_t capacity = 0;

        if (!read_value(sharedString + StringOffset + 0x10, length) || !read_value(sharedString + StringOffset + 0x18, capacity))
            return false;

        if (length > MaximumLength || length > capacity)
            return false;

        if (length == 0)
            return true;

        uintptr_t dataAddress = sharedString + StringOffset;
        if (capacity > 0xF)
        {
            if (!read_value(sharedString + StringOffset, dataAddress) || !dataAddress)
                return false;
        }

        if (dataAddress > UINTPTR_MAX - (length - 1) || !can_read(dataAddress, length))
            return false;

        out.assign(reinterpret_cast<const char*>(dataAddress), length);
        return true;
    }

    inline bool push_property_value_direct_impl(lua_State* L, void* getset, void* inst, uintptr_t getter_addr, int prop_type)
    {
        __try
        {
            switch (prop_type)
            {
            case 1:
            {
                using Fn = bool(__fastcall*)(void*, void*);
                lua_pushboolean(L, reinterpret_cast<Fn>(getter_addr)(getset, inst) ? 1 : 0);
                return true;
            }
            case 2:
            {
                using Fn = int(__fastcall*)(void*, void*);
                lua_pushinteger(L, reinterpret_cast<Fn>(getter_addr)(getset, inst));
                return true;
            }
            case 4:
            {
                using Fn = float(__fastcall*)(void*, void*);
                lua_pushnumber(L, reinterpret_cast<Fn>(getter_addr)(getset, inst));
                return true;
            }
            case 5:
            {
                using Fn = double(__fastcall*)(void*, void*);
                lua_pushnumber(L, reinterpret_cast<Fn>(getter_addr)(getset, inst));
                return true;
            }
            case 29:
            {
                using Fn = void(__fastcall*)(void*, void*, void*);
                uint64_t tmp = 0;
                reinterpret_cast<Fn>(getter_addr)(getset, &tmp, inst);
                lua_pushinteger(L, static_cast<int>(static_cast<uint32_t>(tmp)));
                return true;
            }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }

    constexpr int Vector3int16Type = 14;
    constexpr int Color3uint8Type = 18;
    constexpr int SystemAddressType = 29;
    constexpr int OptionalCoordinateFrameType = 71;
    constexpr int CSGPropertyDataType = 72;
    constexpr int ReplicationPVType = 89;
    constexpr int FacsReplicationDataType = 90;
    constexpr int NetAssetRefType = 94;
    constexpr int AssetContentMapType = 97;
    constexpr int AnimTrackPlayStateType = 101;
    constexpr int AnimTrackMetadataType = 102;
    constexpr int AnimTrackWeightType = 103;

    inline void engine_free(void* block)
    {
        using Fn = void(__fastcall*)(void*);
        __try
        {
            reinterpret_cast<Fn>(Offsets::PropertyValue::OperatorDelete)(block);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    inline void engine_destroy(uintptr_t destructor, void* value)
    {
        using Fn = void(__fastcall*)(void*);
        __try
        {
            reinterpret_cast<Fn>(destructor)(value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    inline void release_shared_count(uintptr_t control)
    {
        if (!control)
            return;

        using Fn = void(__fastcall*)(void*);
        __try
        {
            auto* uses = reinterpret_cast<volatile long*>(control + 8);
            auto* weaks = reinterpret_cast<volatile long*>(control + 12);
            void** vtable = *reinterpret_cast<void***>(control);
            if (_InterlockedDecrement(uses) == 0)
            {
                reinterpret_cast<Fn>(vtable[0])(reinterpret_cast<void*>(control));
                if (_InterlockedDecrement(weaks) == 0)
                    reinterpret_cast<Fn>(vtable[1])(reinterpret_cast<void*>(control));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    inline void free_engine_vector(uintptr_t first, uintptr_t capacityEnd)
    {
        if (!first)
            return;

        uintptr_t block = first;
        if (capacityEnd - first >= 0x1000)
            block = seh_read_ptr(first - 8);

        (void)block;
    }

    inline bool read_engine_string(uintptr_t address, std::string& out)
    {
        out.clear();
        size_t size = 0;
        size_t capacity = 0;
        if (!read_value(address + 0x10, size) || !read_value(address + 0x18, capacity) || size > (16u << 20))
            return false;

        uintptr_t data = address;
        if (capacity >= 0x10 && !read_value(address, data))
            return false;

        if (size && !can_read(data, size))
            return false;

        out.assign(reinterpret_cast<const char*>(data), size);
        return true;
    }

    inline void push_hex(lua_State* L, const unsigned char* bytes, size_t count)
    {
        static constexpr char Digits[] = "0123456789abcdef";
        std::string hex(count * 2, '0');
        for (size_t i = 0; i < count; ++i)
        {
            hex[i * 2] = Digits[bytes[i] >> 4];
            hex[i * 2 + 1] = Digits[bytes[i] & 0xF];
        }
        lua_pushlstring(L, hex.data(), hex.size());
    }

    inline bool call_struct_getter_seh(uintptr_t getterAddress, void* getset, void* instance, void* out)
    {
        using Fn = void*(__fastcall*)(void*, void*, void*);
        __try
        {
            reinterpret_cast<Fn>(getterAddress)(getset, out, instance);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool read_cframe_seh(uintptr_t address, float (&out)[12])
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(out));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool push_constructed(lua_State* L, const char* library, const char* constructor, const double* args, int count)
    {
        const int top = lua_gettop(L);
        lua_getglobal(L, library);
        if (!lua_istable(L, -1) && !lua_isuserdata(L, -1))
        {
            lua_settop(L, top);
            return false;
        }

        lua_getfield(L, -1, constructor);
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1))
        {
            lua_settop(L, top);
            return false;
        }

        for (int i = 0; i < count; ++i)
            lua_pushnumber(L, args[i]);

        if (lua_pcall(L, count, 1, 0) != LUA_OK)
        {
            lua_settop(L, top);
            return false;
        }

        return true;
    }

    inline void set_number_field(lua_State* L, const char* name, double value)
    {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, name);
    }

    inline void set_boolean_field(lua_State* L, const char* name, bool value)
    {
        lua_pushboolean(L, value);
        lua_setfield(L, -2, name);
    }

    inline void set_vector3_field(lua_State* L, const char* name, const float* xyz)
    {
        const double args[3] = { xyz[0], xyz[1], xyz[2] };
        if (push_constructed(L, "Vector3", "new", args, 3))
            lua_setfield(L, -2, name);
    }

    struct CopiedAssetContentMap
    {
        std::vector<std::pair<std::string, std::string>> Entries;
        bool Valid = false;
    };

    inline CopiedAssetContentMap read_asset_content_map(const unsigned char* map)
    {
        CopiedAssetContentMap result;
        uint32_t count = 0;
        uintptr_t control = 0;
        uintptr_t entries = 0;
        uint32_t log2Buckets = 0;
        std::memcpy(&count, map + 0, sizeof(count));
        std::memcpy(&control, map + 8, sizeof(control));
        std::memcpy(&entries, map + 16, sizeof(entries));
        std::memcpy(&log2Buckets, map + 28, sizeof(log2Buckets));

        result.Valid = true;
        if (!count || !control || !entries || log2Buckets >= 32)
            return result;

        const uintptr_t header = control + 4 * (uintptr_t(1) << log2Buckets);
        uint32_t slots = 0;
        uint32_t headerWords = 0;
        if (!read_value(header + 4, slots) || !read_value(header + 16, headerWords) || slots > (1u << 20))
            return result;

        const uintptr_t bitmap = header + 8 * uintptr_t(headerWords) + 24;
        for (uint32_t slot = 0; slot < slots; ++slot)
        {
            uint64_t word = 0;
            if (!read_value(bitmap + 8 * uintptr_t(slot >> 6), word))
                break;
            if (word & (uint64_t(1) << (slot & 63)))
                continue;

            std::string key, value;
            const uintptr_t entry = entries + uintptr_t(slot) * 80;
            if (read_engine_string(entry, key) && read_engine_string(entry + 32, value))
                result.Entries.emplace_back(std::move(key), std::move(value));
        }

        return result;
    }

    inline bool push_opaque_value(lua_State* L, void* getset, void* instance, uintptr_t getterAddress, int type)
    {
        alignas(16) unsigned char buffer[128] = {};
        if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
            return false;

        switch (type)
        {
        case ReplicationPVType:
        {
            float f[16];
            std::memcpy(f, buffer, sizeof(f));

            const float* c0 = f + 3;
            const float* c1 = f + 6;
            float c2[3] = {
                c0[1] * c1[2] - c0[2] * c1[1],
                c0[2] * c1[0] - c0[0] * c1[2],
                c0[0] * c1[1] - c0[1] * c1[0],
            };

            double cf[12] = { f[0], f[1], f[2], c0[0], c1[0], c2[0], c0[1], c1[1], c2[1], c0[2], c1[2], c2[2] };
            if (c0[0] == 0 && c0[1] == 0 && c0[2] == 0 && c1[0] == 0 && c1[1] == 0 && c1[2] == 0)
            {
                const double identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                std::memcpy(cf + 3, identity, sizeof(identity));
            }

            lua_createtable(L, 0, 3);
            if (push_constructed(L, "CFrame", "new", cf, 12))
                lua_setfield(L, -2, "CFrame");
            set_vector3_field(L, "LinearVelocity", f + 10);
            set_vector3_field(L, "AngularVelocity", f + 13);
            return true;
        }
        case FacsReplicationDataType:
        {
            uint32_t header = 0;
            float values[6];
            uintptr_t first = 0, last = 0, capacityEnd = 0;
            std::memcpy(&header, buffer, sizeof(header));
            std::memcpy(values, buffer + 4, sizeof(values));
            std::memcpy(&first, buffer + 32, sizeof(first));
            std::memcpy(&last, buffer + 40, sizeof(last));
            std::memcpy(&capacityEnd, buffer + 48, sizeof(capacityEnd));

            std::vector<float> controls;
            if (first && last > first && (last - first) % sizeof(float) == 0 && (last - first) <= (1u << 20) &&
                can_read(first, last - first))
            {
                controls.resize((last - first) / sizeof(float));
                std::memcpy(controls.data(), reinterpret_cast<const void*>(first), last - first);
            }
            free_engine_vector(first, capacityEnd);

            lua_createtable(L, 0, 3);
            set_number_field(L, "Header", header);

            lua_createtable(L, 6, 0);
            for (int i = 0; i < 6; ++i)
            {
                lua_pushnumber(L, values[i]);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "Values");

            lua_createtable(L, static_cast<int>(controls.size()), 0);
            for (size_t i = 0; i < controls.size(); ++i)
            {
                lua_pushnumber(L, controls[i]);
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            lua_setfield(L, -2, "Controls");
            return true;
        }
        case CSGPropertyDataType:
        {
            uintptr_t payload = 0, control = 0;
            unsigned char key[8];
            std::memcpy(&payload, buffer, sizeof(payload));
            std::memcpy(&control, buffer + 8, sizeof(control));
            std::memcpy(key, buffer + 16, sizeof(key));
            release_shared_count(control);

            lua_createtable(L, 0, 2);
            set_boolean_field(L, "HasData", payload != 0);
            push_hex(L, key, sizeof(key));
            lua_setfield(L, -2, "Key");
            return true;
        }
        case NetAssetRefType:
        {
            uintptr_t store = 0;
            unsigned char key[24];
            const unsigned char flags = buffer[40];
            std::memcpy(&store, buffer, sizeof(store));
            std::memcpy(key, buffer + 16, sizeof(key));
            engine_destroy(Offsets::PropertyValue::NetAssetRefDtor, buffer);

            lua_createtable(L, 0, 3);
            push_hex(L, key, sizeof(key));
            lua_setfield(L, -2, "Key");
            set_number_field(L, "Flags", flags);
            set_boolean_field(L, "HasStore", store != 0);
            return true;
        }
        case AssetContentMapType:
        {
            CopiedAssetContentMap map = read_asset_content_map(buffer);
            engine_destroy(Offsets::PropertyValue::AssetContentMapDtor, buffer);

            lua_createtable(L, 0, static_cast<int>(map.Entries.size()));
            for (const auto& [key, value] : map.Entries)
            {
                lua_pushlstring(L, key.data(), key.size());
                lua_pushlstring(L, value.data(), value.size());
                lua_settable(L, -3);
            }
            return true;
        }
        case AnimTrackPlayStateType:
        {
            double timestamp = 0;
            float speed = 0;
            std::memcpy(&timestamp, buffer, sizeof(timestamp));
            std::memcpy(&speed, buffer + 8, sizeof(speed));

            lua_createtable(L, 0, 3);
            set_number_field(L, "Timestamp", timestamp);
            set_number_field(L, "Speed", speed);
            set_boolean_field(L, "IsPlaying", buffer[12] != 0);
            return true;
        }
        case AnimTrackWeightType:
        {
            double timestamp = 0;
            float weight = 0, fade = 0;
            std::memcpy(&timestamp, buffer, sizeof(timestamp));
            std::memcpy(&weight, buffer + 8, sizeof(weight));
            std::memcpy(&fade, buffer + 12, sizeof(fade));

            lua_createtable(L, 0, 3);
            set_number_field(L, "Timestamp", timestamp);
            set_number_field(L, "WeightTarget", weight);
            set_number_field(L, "FadeTime", fade);
            return true;
        }
        case AnimTrackMetadataType:
        {
            int16_t priority = 0;
            std::memcpy(&priority, buffer, sizeof(priority));

            lua_createtable(L, 0, 2);
            set_number_field(L, "Priority", priority);
            set_boolean_field(L, "Looped", buffer[2] != 0);
            return true;
        }
        }

        return false;
    }

    inline bool is_opaque_type(int type)
    {
        switch (type)
        {
        case CSGPropertyDataType:
        case ReplicationPVType:
        case FacsReplicationDataType:
        case NetAssetRefType:
        case AssetContentMapType:
        case AnimTrackPlayStateType:
        case AnimTrackMetadataType:
        case AnimTrackWeightType:
            return true;
        }
        return false;
    }

    inline bool is_unbridged_type(int type)
    {
        return type == Color3uint8Type || type == Vector3int16Type || type == SystemAddressType ||
            type == OptionalCoordinateFrameType || is_opaque_type(type);
    }

    inline bool push_unbridged_value(lua_State* L, void* getset, void* instance, uintptr_t getterAddress, int type)
    {
        if (is_opaque_type(type))
            return push_opaque_value(L, getset, instance, getterAddress, type);

        alignas(16) unsigned char buffer[64] = {};

        switch (type)
        {
        case Color3uint8Type:
        {
            if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
                return false;

            const double rgb[3] = { double(buffer[0]), double(buffer[1]), double(buffer[2]) };
            return push_constructed(L, "Color3", "fromRGB", rgb, 3);
        }
        case Vector3int16Type:
        {
            if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
                return false;

            int16_t xyz[3];
            std::memcpy(xyz, buffer, sizeof(xyz));
            const double args[3] = { double(xyz[0]), double(xyz[1]), double(xyz[2]) };
            return push_constructed(L, "Vector3int16", "new", args, 3);
        }
        case SystemAddressType:
        {
            if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
                return false;

            uint32_t address = 0;
            std::memcpy(&address, buffer, sizeof(address));
            lua_pushnumber(L, address);
            return true;
        }
        case OptionalCoordinateFrameType:
        {
            if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
                return false;

            uintptr_t tagged = 0;
            std::memcpy(&tagged, buffer, sizeof(tagged));
            if ((tagged & 2) == 0)
            {
                lua_pushnil(L);
                return true;
            }

            float cf[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
            const uintptr_t frame = tagged & ~uintptr_t(3);
            const bool read = !frame || read_cframe_seh(frame, cf);
            if (!read)
                return false;

            const double args[12] = {
                cf[9], cf[10], cf[11],
                cf[0], cf[1], cf[2],
                cf[3], cf[4], cf[5],
                cf[6], cf[7], cf[8],
            };
            return push_constructed(L, "CFrame", "new", args, 12);
        }
        }

        return false;
    }

    constexpr int BinaryStringType = 30;
    constexpr int SharedStringType = 63;

    // BinaryString's getter fills an engine std::string (MSVC layout: inline buffer or heap ptr at
    // +0, size +0x10, capacity +0x18). Read it out of a raw buffer and hand a heap block back to
    // the engine's own operator delete - never let our CRT free engine memory.
    inline bool push_engine_string(lua_State* L, uintptr_t getterAddress, void* getset, void* instance)
    {
        alignas(16) unsigned char buffer[32] = {};
        if (!call_struct_getter_seh(getterAddress, getset, instance, buffer))
            return false;

        size_t size = 0;
        size_t capacity = 0;
        std::memcpy(&size, buffer + 0x10, sizeof(size));
        std::memcpy(&capacity, buffer + 0x18, sizeof(capacity));
        if (size > capacity || size > 64ull * 1024ull * 1024ull)
            return false;

        if (capacity <= 0xF)
        {
            lua_pushlstring(L, reinterpret_cast<const char*>(buffer), size);
            return true;
        }

        uintptr_t data = 0;
        std::memcpy(&data, buffer, sizeof(data));
        if (!data || !can_read(data, size ? size : 1))
            return false;

        lua_pushlstring(L, reinterpret_cast<const char*>(data), size);
        engine_free(reinterpret_cast<void*>(data));
        return true;
    }

    // Only the types the engine's own __index pusher cannot produce; everything else goes through
    // the scriptable-flag + __index path in gethiddenproperty. The getset comes from the build's
    // descriptor offset (the old hardcoded +0x90 is wrong on 744).
    inline bool push_property_value_direct(lua_State* L, uintptr_t instance, uintptr_t prop_desc)
    {
        const PropertyAccess access = get_property_access(prop_desc);
        const int propType = access.Type;
        if (propType != BinaryStringType && propType != SharedStringType && !is_unbridged_type(propType))
            return false;

        const uintptr_t getterAddress = access.Getter;
        const uintptr_t getsetAddress = access.GetSet;
        if (!getterAddress || !getsetAddress)
            return false;

        void* getset = reinterpret_cast<void*>(getsetAddress);
        void* inst = reinterpret_cast<void*>(instance);

        if (propType == BinaryStringType)
            return push_engine_string(L, getterAddress, getset, inst);

        if (propType == SharedStringType)
        {
            std::string value;
            if (!call_content_getter_seh(getterAddress, getset, inst, value))
                return false;
            lua_pushlstring(L, value.data(), value.size());
            return true;
        }

        return push_unbridged_value(L, getset, inst, getterAddress, propType);
    }

    inline bool is_hidden_from_game_scripts(uintptr_t prop, uint32_t flags)
    {
        if ((flags & 0x10) == 0)
            return true;

        const uint64_t required = seh_read_ptr(prop + Offsets::PropertyDescriptor::ReadCapabilities);
        if (required == 0)
            return false;

        int identity = 2;
        const uint64_t gameCapabilities = 0xFFFFFFFFFFFFFF00ULL | Roblox::GetCapabilities(&identity);
        return (required & gameCapabilities) != required;
    }

    inline bool lookup_scriptable_override(uintptr_t inst, const char* prop_name, bool& out_state)
    {
        if (scriptable_overrides.empty() || !inst || !prop_name)
            return false;

        const auto instIt = scriptable_overrides.find(inst);
        if (instIt == scriptable_overrides.end())
            return false;

        const auto propIt = instIt->second.find(prop_name);
        if (propIt == instIt->second.end())
            return false;

        out_state = propIt->second.Scriptable;
        return true;
    }

    struct ScopedScriptable
    {
        uintptr_t Descriptor = 0;
        uint32_t OriginalFlags = 0;
        bool Applied = false;

        ScopedScriptable(uintptr_t inst, const char* prop_name)
        {
            if (scriptable_overrides.empty() || !inst || !prop_name)
                return;

            const auto instIt = scriptable_overrides.find(inst);
            if (instIt == scriptable_overrides.end())
                return;

            const auto propIt = instIt->second.find(prop_name);
            if (propIt == instIt->second.end() || !propIt->second.Descriptor)
                return;

            const uintptr_t descriptor = propIt->second.Descriptor;
            const uintptr_t flagsAddress = descriptor + Offsets::Instance::PropertyDescriptorBitFlags;
            if (!can_read(flagsAddress, sizeof(uint32_t)))
                return;

            const uint32_t flags = seh_read_u32(flagsAddress);
            const uint32_t wanted = propIt->second.Scriptable
                ? (flags | 0x10u)
                : (flags & ~0x10u);

            if (wanted == flags)
                return;

            if (!seh_write_u32(flagsAddress, wanted))
                return;

            Descriptor = descriptor;
            OriginalFlags = flags;
            Applied = true;
        }

        ~ScopedScriptable()
        {
            if (Applied)
                seh_write_u32(Descriptor + Offsets::Instance::PropertyDescriptorBitFlags, OriginalFlags);
        }

        ScopedScriptable(const ScopedScriptable&) = delete;
        ScopedScriptable& operator=(const ScopedScriptable&) = delete;
    };

    inline uintptr_t instance_from_userdata(lua_State* L, int index)
    {
        if (!lua_isuserdata(L, index))
            return 0;

        void* userdata = lua_touserdata(L, index);
        if (!userdata)
            return 0;

        return seh_read_ptr(reinterpret_cast<uintptr_t>(userdata));
    }

    inline int isscriptable(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* prop_name = luaL_checkstring(L, 2);

        uintptr_t inst = 0, prop = 0;
        if (!resolve_property(L, inst, prop) || !prop) {
            lua_pushboolean(L, false);
            return 1;
        }

        bool overridden = false;
        if (lookup_scriptable_override(inst, prop_name, overridden)) {
            lua_pushboolean(L, overridden);
            return 1;
        }

        lua_pushboolean(L, read_scriptable_flag(prop));
        return 1;
    }

    inline int setscriptable(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* prop_name = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TBOOLEAN);

        uintptr_t inst = 0, prop = 0;
        if (!resolve_property(L, inst, prop) || !prop) {
            lua_pushboolean(L, false);
            return 1;
        }

        bool wasScriptable = read_scriptable_flag(prop);
        lookup_scriptable_override(inst, prop_name, wasScriptable);

        const bool state = lua_toboolean(L, 3) != 0;

        if (state == read_scriptable_flag(prop))
        {
            const auto instIt = scriptable_overrides.find(inst);
            if (instIt != scriptable_overrides.end())
            {
                instIt->second.erase(prop_name);
                if (instIt->second.empty())
                    scriptable_overrides.erase(instIt);
            }
        }
        else
        {
            scriptable_overrides[inst][prop_name] = ScriptableOverride{ prop, state };
        }

        lua_pushboolean(L, wasScriptable);
        return 1;
    }

    struct WriteOnlyAlias
    {
        const char* WriteOnly;
        const char* Readable;
    };

    inline constexpr WriteOnlyAlias WriteOnlyAliases[] = {
        { "siz", "Size" },
        { "shap", "Shape" },
    };

    inline bool push_writeonly_alias_value(lua_State* L, int instanceIndex, const char* propertyName)
    {
        if (!propertyName)
            return false;

        for (const WriteOnlyAlias& alias : WriteOnlyAliases)
        {
            if (std::strcmp(alias.WriteOnly, propertyName) != 0)
                continue;

            const int top = lua_gettop(L);
            lua_pushcfunction(L, protected_getfield, "gethiddenproperty_alias");
            lua_pushvalue(L, instanceIndex);
            lua_pushstring(L, alias.Readable);

            if (lua_pcall(L, 2, 1, 0) == LUA_OK && !lua_isnil(L, -1))
                return true;

            lua_settop(L, top);
            return false;
        }

        return false;
    }

    inline int gethiddenproperty(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* propertyName = luaL_checkstring(L, 2);

        uintptr_t instance = 0;
        uintptr_t propertyDescriptor = 0;

        if (!resolve_property(L, instance, propertyDescriptor) || !instance || !propertyDescriptor)
        {
            int top = lua_gettop(L);
            lua_pushcfunction(L, protected_getfield, "Fallback");
            lua_pushvalue(L, 1);
            lua_pushstring(L, propertyName);
            if (lua_pcall(L, 2, 1, 0) == LUA_OK)
            {
                lua_pushboolean(L, false);
                return 2;
            }
            lua_settop(L, top);
            if (!push_writeonly_alias_value(L, 1, propertyName))
                lua_pushnil(L);
            lua_pushboolean(L, true);
            return 2;
        }

        uint32_t originalFlags = 0;
        if (!seh_try_read_u32(propertyDescriptor + Offsets::Instance::PropertyDescriptorBitFlags, originalFlags))
        {
            lua_pushnil(L);
            lua_pushboolean(L, false);
            return 2;
        }

        const bool wasScriptable = (originalFlags & 0x10) != 0;
        const bool wasHidden = is_hidden_from_game_scripts(propertyDescriptor, originalFlags);

        if (push_property_value_direct(L, instance, propertyDescriptor))
        {
            lua_pushboolean(L, wasHidden);
            return 2;
        }

        const uintptr_t flagsAddress = propertyDescriptor + Offsets::Instance::PropertyDescriptorBitFlags;
        bool flagsChanged = false;

        __try
        {
            *reinterpret_cast<uint32_t*>(flagsAddress) = originalFlags | 0x10;
            flagsChanged = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        int top = lua_gettop(L);
        lua_pushcfunction(L, protected_getfield, "gethiddenproperty");
        lua_pushvalue(L, 1);
        lua_pushstring(L, propertyName);

        int status = lua_pcall(L, 2, 1, 0);

        __try
        {
            if (flagsChanged)
                *reinterpret_cast<uint32_t*>(flagsAddress) = originalFlags;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        if (status != LUA_OK)
        {
            lua_settop(L, top);
            lua_pushnil(L);
        }

        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            if (!push_writeonly_alias_value(L, 1, propertyName))
                lua_pushnil(L);
        }

        lua_pushboolean(L, wasHidden);
        return 2;
    }

    inline int sethiddenproperty(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* propertyName = luaL_checkstring(L, 2);
        luaL_checkany(L, 3);

        uintptr_t instance = 0;
        uintptr_t propertyDescriptor = 0;

        if (!resolve_property(L, instance, propertyDescriptor) || !instance || !propertyDescriptor)
        {
            bool didSet = set_property_value(L, 1, 2, 3);
            lua_pushboolean(L, false);
            return 1;
        }

        uint32_t originalFlags = 0;
        if (!seh_try_read_u32(propertyDescriptor + Offsets::Instance::PropertyDescriptorBitFlags, originalFlags))
        {
            lua_pushboolean(L, false);
            return 1;
        }

        const bool wasScriptable = (originalFlags & 0x10) != 0;
        const uintptr_t flagsAddress = propertyDescriptor + Offsets::Instance::PropertyDescriptorBitFlags;

        bool flagsChanged = false;
        if (!wasScriptable)
        {
            __try
            {
                *reinterpret_cast<uint32_t*>(flagsAddress) = originalFlags | 0x10;
                flagsChanged = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        bool didSet = set_property_value(L, 1, 2, 3);

        if (flagsChanged)
        {
            __try
            {
                *reinterpret_cast<uint32_t*>(flagsAddress) = originalFlags;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        lua_pushboolean(L, !wasScriptable && didSet);
        return 1;
    }

    inline int gethiddenproperties(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        void* userdata = lua_touserdata(L, 1);
        if (!userdata || !can_read(reinterpret_cast<uintptr_t>(userdata)))
        {
            lua_newtable(L);
            return 1;
        }

        uintptr_t inst = *reinterpret_cast<uintptr_t*>(userdata);
        if (!inst)
        {
            lua_newtable(L);
            return 1;
        }

        std::vector<uintptr_t> all_props = get_all_properties_for_instance(inst);

        lua_newtable(L);
        int result_table = lua_gettop(L);
        std::unordered_set<std::string> added;

        for (uintptr_t prop_desc : all_props)
        {
            const char* prop_name = get_property_name(prop_desc);
            if (!prop_name || !prop_name[0])
                continue;

            if (added.find(prop_name) != added.end())
                continue;

            bool is_scriptable = read_scriptable_flag(prop_desc);
            if (is_scriptable)
                continue;

            added.insert(prop_name);

            lua_pushcfunction(L, gethiddenproperty, "gethiddenproperty");
            lua_pushvalue(L, 1);
            lua_pushstring(L, prop_name);

            if (lua_pcall(L, 2, 2, 0) == LUA_OK)
            {
                bool is_hidden = lua_toboolean(L, -1);
                lua_pop(L, 1);

                if (!lua_isnil(L, -1))
                {
                    lua_setfield(L, result_table, prop_name);
                }
                else
                {
                    lua_pop(L, 1);
                }
            }
            else
            {
                lua_pop(L, 1);
            }
        }

        return 1;
    }

    inline int getproperties(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);

        void* userdata = lua_touserdata(L, 1);
        if (!userdata || !can_read(reinterpret_cast<uintptr_t>(userdata)))
        {
            lua_newtable(L);
            return 1;
        }

        uintptr_t inst = *reinterpret_cast<uintptr_t*>(userdata);
        if (!inst)
        {
            lua_newtable(L);
            return 1;
        }

        std::vector<uintptr_t> all_props = get_all_properties_for_instance(inst);

        lua_newtable(L);
        int result_table = lua_gettop(L);
        std::unordered_set<std::string> added;

        for (uintptr_t prop_desc : all_props)
        {
            const char* prop_name = get_property_name(prop_desc);
            if (!prop_name || !prop_name[0])
                continue;

            if (added.find(prop_name) != added.end())
                continue;
            added.insert(prop_name);

            int top = lua_gettop(L);
            lua_pushcfunction(L, protected_getfield, "GetPropertiesArea");
            lua_pushvalue(L, 1);
            lua_pushstring(L, prop_name);
            if (lua_pcall(L, 2, 1, 0) == LUA_OK)
            {
                if (!lua_isnil(L, -1))
                {
                    lua_setfield(L, result_table, prop_name);
                }
                else
                {
                    lua_pop(L, 1);
                }
            }
            else
            {
                lua_settop(L, top);
            }
        }

        return 1;
    }

    int getcallbackvalue(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        luaL_checktype(L, 2, LUA_TSTRING);

        uintptr_t TrackedInstance = 0;
        void* Userdata = lua_touserdata(L, 1);

        if (Userdata)
            read_value(reinterpret_cast<uintptr_t>(Userdata), TrackedInstance);

        const char* PropertyName = lua_tostring(L, 2);

        if (TrackedInstance && PropertyName)
        {
            const auto InstanceIt = callback_values.find(TrackedInstance);

            if (InstanceIt != callback_values.end())
            {
                const auto CallbackIt = InstanceIt->second.find(PropertyName);

                if (CallbackIt != InstanceIt->second.end() && CallbackIt->second.Generation == SharedVariables::StateGeneration.load())
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, CallbackIt->second.FunctionRef);

                    if (lua_isfunction(L, -1))
                        return 1;

                    lua_pop(L, 1);
                }
            }
        }

        uintptr_t Instance = 0;
        uintptr_t Descriptor = 0;

        if (!resolve_property(L, Instance, Descriptor))
        {
            lua_pushnil(L);
            lua_pushstring(L, "The Callback property doesnt exist.");
            return 2;
        }

        int32_t RelativeOffset = 0;

        {
            static bool Dumped = false;
            std::uint32_t Probe = 0;
            if (!Dumped && (!read_value(Descriptor + Offsets::Properties::CallbackOffsetInDescriptor, Probe) || Probe == 0))
            {
                Dumped = true;
                SUPQ_LOG_ERR("property walk: CallbackOffsetInDescriptor(+0x%llX) unreadable/zero on descriptor 0x%llX",
                    (unsigned long long)Offsets::Properties::CallbackOffsetInDescriptor,
                    (unsigned long long)Descriptor);
                SUPQ_DUMP("PropertyDescriptor", Descriptor, 0x100);
            }
        }

        if (!read_value(Descriptor + Offsets::Properties::CallbackOffsetInDescriptor, RelativeOffset) || RelativeOffset == 0)
        {
            lua_pushnil(L);
            lua_pushstring(L, "descriptor carries no callback offset");
            return 2;
        }

        const uintptr_t Base = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(Instance) + static_cast<std::intptr_t>(RelativeOffset));

        const uintptr_t Candidates[] = {
            Base,
            Base >= Offsets::Properties::AlternateHolderAdjustment
                ? Base - Offsets::Properties::AlternateHolderAdjustment
                : 0,
        };

        const char* furthest = "holder unreadable";

        for (const uintptr_t Holder : Candidates)
        {
            if (!Holder)
                continue;

            uintptr_t Owner = 0;
            uintptr_t Callback = 0;
            uintptr_t LiveThreadRef = 0;
            uint32_t FunctionRef = 0;

            if (!read_value(Holder + Offsets::Properties::CallbackOwnerOffset, Owner) || !Owner)
                continue;

            furthest = "callback owner has no function";
            if (!read_value(Owner + Offsets::Properties::CallbackFunctionOffset, Callback) || !Callback)
                continue;

            furthest = "callback has no thread reference";
            if (!read_value(Callback + Offsets::Properties::LiveThreadRefOffset, LiveThreadRef) || !LiveThreadRef)
                continue;

            furthest = "thread reference carries no registry id";
            if (!read_value(LiveThreadRef + Offsets::Properties::FunctionRefIdOffset, FunctionRef) || FunctionRef == 0)
                continue;

            uintptr_t CallbackThread = 0;
            uintptr_t CallbackGlobal = 0;
            uintptr_t CurrentGlobal = 0;

            if (read_value(LiveThreadRef + Offsets::Properties::CallbackThreadOffset, CallbackThread) && CallbackThread &&
                read_value(CallbackThread + Offsets::Properties::LuaStateGlobalOffset, CallbackGlobal) && CallbackGlobal &&
                read_value(reinterpret_cast<uintptr_t>(L) + Offsets::Properties::LuaStateGlobalOffset, CurrentGlobal) &&
                CallbackGlobal != CurrentGlobal)
            {
                furthest = "callback belongs to another Luau VM";
                continue;
            }

            furthest = "registry slot is not a function";
            lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<int>(FunctionRef));
            if (lua_isfunction(L, -1))
                return 1;

            lua_pop(L, 1);
        }

        lua_pushnil(L);
        lua_pushstring(L, furthest);
        return 2;
    }

    inline int setcallbackvalue(lua_State* L)
    {
        luaL_checktype(L, 1, LUA_TUSERDATA);
        const char* prop = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        lua_settop(L, 3);

        lua_pushcfunction(L, protected_setfield, "setcallbackvalue");
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 3);
        lua_call(L, 3, 0);

        int top = lua_gettop(L);
        lua_pushstring(L, "__callbacks");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);

            lua_newtable(L);
            lua_pushstring(L, "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);

            lua_pushstring(L, "__callbacks");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }

        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);

            lua_pushvalue(L, 1);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }

        lua_pushstring(L, prop);
        lua_pushvalue(L, 3);
        lua_rawset(L, -3);

        TrackCallbackValue(L, 1, 2, 3);

        lua_settop(L, top);
        return 0;
    }

    inline void RegisterLibrary(lua_State* L)
    {
        if (!L) return;

        Utils::AddFunction(L, "isscriptable", isscriptable);
        Utils::AddFunction(L, "is_scriptable", isscriptable);

        Utils::AddFunction(L, "setscriptable", setscriptable);
        Utils::AddFunction(L, "set_scriptable", setscriptable);

        Utils::AddFunction(L, "gethiddenproperty", gethiddenproperty);
        Utils::AddFunction(L, "get_hidden_property", gethiddenproperty);
        Utils::AddFunction(L, "gethiddenprop", gethiddenproperty);
        Utils::AddFunction(L, "get_hidden_prop", gethiddenproperty);

        Utils::AddFunction(L, "sethiddenproperty", sethiddenproperty);
        Utils::AddFunction(L, "set_hidden_property", sethiddenproperty);
        Utils::AddFunction(L, "sethiddenprop", sethiddenproperty);
        Utils::AddFunction(L, "set_hidden_prop", sethiddenproperty);

        Utils::AddFunction(L, "gethiddenproperties", gethiddenproperties);
        Utils::AddFunction(L, "get_hidden_properties", gethiddenproperties);

        Utils::AddFunction(L, "getproperties", getproperties);
        Utils::AddFunction(L, "get_properties", getproperties);

        Utils::AddFunction(L, "getcallbackvalue", getcallbackvalue);
        Utils::AddFunction(L, "get_callback_value", getcallbackvalue);
        Utils::AddFunction(L, "getcallbackmember", getcallbackvalue);
        Utils::AddFunction(L, "get_callback_member", getcallbackvalue);

        Utils::AddFunction(L, "setcallbackvalue", setcallbackvalue);
        Utils::AddFunction(L, "set_callback_value", setcallbackvalue);
        Utils::AddFunction(L, "setcallbackmember", setcallbackvalue);
        Utils::AddFunction(L, "set_callback_member", setcallbackvalue);
    }
}
