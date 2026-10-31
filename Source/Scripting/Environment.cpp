#include <Runtime/Execution/ExecutionGuard.hpp>
#include <Security/BrandGuard.hpp>
#include <Security/CloneGuard.hpp>
#include <Core/DebugLog.hpp>
#include <Core/CallTrace.hpp>
#include <Scripting/Obfuscation.hpp>
#include "Scripting/Environment.hpp"
#include <cstring>
#include <Scripting/Bridge/SafeCall.hpp>
#include <Scripting/Environment.hpp>
#include <Scripting/Modules/Http.hpp>
#include <Scripting/Modules/Cache.hpp>
#include <Scripting/Modules/Crypt.hpp>
#include <Scripting/Modules/Input.hpp>
#include <Scripting/Modules/Debug.hpp>
#include <Scripting/Modules/Script.hpp>
#include <Scripting/Modules/Closures.hpp>
#include <Scripting/Modules/Instance.hpp>
#include <Scripting/Modules/Websocket.hpp>
#include <Scripting/Modules/Metatable.hpp>
#include <Scripting/Modules/Filesystem.hpp>
#include <Scripting/Modules/Miscellaneous.hpp>
#include <Scripting/Modules/Trampoline.hpp>
#include <Scripting/Modules/Signals.hpp>
#include <Scripting/Modules/Signal.hpp>
#include <Scripting/Modules/Reflection.hpp>
#include <Scripting/Modules/Regex.hpp>
#include <Scripting/Modules/FileDialog.hpp>
#include <Scripting/Modules/Compat.hpp>
#include <Scripting/Modules/Actor.hpp>
#include <Scripting/Modules/Websocket.hpp>
#include <Network/Raknet/Raknet.hpp>
#include <Scripting/Modules/Oth.hpp>
#include <Scripting/Modules/Drawing.h>
#include <Scripting/Modules/ImGuiLib.hpp>
#include <Scripting/Modules/Console.h>
#include <Scripting/Modules/Bit.h>
#include <Scripting/Modules/ModuleLib.h> 
#include <Scripting/InitScripts.hpp>
#include <Security/PurchaseGuard.hpp>
#include "Update/FFlags.hpp"

lua_CFunction OriginalIndex = nullptr;
lua_CFunction OriginalNamecall = nullptr;
lua_CFunction OriginalNewindex = nullptr;
Closure* GameIndexClosure = nullptr;
Closure* GameNamecallClosure = nullptr;
Closure* GameNewindexClosure = nullptr;
namespace Environment {
    std::vector<Closure*> function_array;
}

std::vector<const char*> UnsafeFunction = {
    "Load", "OpenScreenshotsFolder", "OpenVideosFolder", "OpenFolder",
    "AddCoreScriptLocal", "SaveScriptProfilingData",
    "GetRobuxBalance", "PerformPurchase", "PerformPurchaseV2", "PerformBulkPurchase",
    "PerformSubscriptionPurchase", "PerformSubscriptionPurchaseV2", "PerformCancelSubscription",
    "PrepareCollectiblesPurchase", "GetUserSubscriptionPaymentHistoryAsync",
    "GetUserSubscriptionDetailsInternalAsync", "GetUserSubscriptionStatusAsync",
    "ReportAbuse", "ReportAbuseV3",
    "TakeScreenshot", "ToggleRecording",
    "OpenBrowserWindow", "OpenNativeOverlay", "ExecuteJavaScript", "ReturnToJavaScript",
    "SendCommand", "EmitHybridEvent", "OpenWeChatAuthWindow",
    "BroadcastNotification", "SetPurchasePromptIsShown",
    "OpenUrl", "DetectUrl", "RegisterLuaUrl", "StartLuaUrlDelivery", "StopLuaUrlDelivery",
    "SwitchToSettingsApp", "SupportsSwitchToSettingsApp", "GetLastLuaUrl",
    "GetAndClearLastPendingUrl", "IsUrlRegistered",
    "RequestInternal", "RequestAsync", "GetAsync", "PostAsync",
    "GetAsyncFullUrl", "PostAsyncFullUrl", "RequestLimitedAsync",
    "HttpRequestAsync", "GetApiV1", "InvokeAsync", "RegisterOpenCloud",
    "CaptureScreenshot", "CreatePostAsync", "DeleteCapture", "DeleteCapturesAsync",
    "GetCaptureFilePathAsync", "GetCaptureUploadDataAsync", "GetCaptureSizeAsync",
    "GetCaptureStorageSizeAsync", "RetrieveCaptures", "SaveCaptureToExternalStorage",
    "SaveCapturesToExternalStorageAsync", "SaveScreenshotCapture",
    "PromptSaveCapturesToGallery", "PromptShareCapture",
    "Call", "GetLast", "GetMessageId", "GetProtocolMethodRequestMessageId",
    "GetProtocolMethodResponseMessageId", "MakeRequest", "Publish",
    "PublishProtocolMethodRequest", "PublishProtocolMethodResponse", "Subscribe",
    "SubscribeToProtocolMethodRequest", "SubscribeToProtocolMethodResponse",
    "GetCredentialsHeaders", "GetDeviceAccessToken", "GetDeviceIntegrityToken",
    "GetDeviceIntegrityTokenYield",
    "NoPromptCreateOutfit", "NoPromptDeleteOutfit", "NoPromptRenameOutfit", "NoPromptSaveAvatar",
    "NoPromptSaveAvatarThumbnailCustomization", "NoPromptSetFavorite", "NoPromptUpdateOutfit",
    "PerformCreateOutfitWithDescription", "PerformDeleteOutfit", "PerformRenameOutfit",
    "PerformSaveAvatarWithDescription", "PerformSetFavorite", "PerformUpdateOutfit",
    "PromptCreateOutfit", "PromptDeleteOutfit", "PromptRenameOutfit", "PromptSaveAvatar",
    "PromptSetFavorite", "PromptUpdateOutfit", "SetAllowInventoryReadAccess",
    "PromptAllowInventoryReadAccess", "SignalCreateOutfitFailed", "SignalCreateOutfitPermissionDenied",
    "SignalDeleteOutfitFailed", "SignalDeleteOutfitPermissionDenied", "SignalRenameOutfitFailed",
    "SignalRenameOutfitPermissionDenied", "SignalSaveAvatarPermissionDenied",
    "SignalSetFavoriteFailed", "SignalSetFavoritePermissionDenied", "SignalUpdateOutfitFailed",
    "SignalUpdateOutfitPermissionDenied",
    "GetLocalFileContents",
    "SetBaseUrl",
    "FetchAssetWithFormat", "RegisterUGCValidationFunction",
    "PerformManagedUpdate",
    "TestService.Run", "TestService", "Run",
};

static bool ContainsEmoji(const std::string& str)
{
    for (size_t i = 0; i < str.size(); i++)
    {
        unsigned char c = (unsigned char)str[i];
        if (c == 0xF0 && i + 3 < str.size())
        {
            unsigned char n1 = (unsigned char)str[i + 1];
            unsigned char n2 = (unsigned char)str[i + 2];
            unsigned char n3 = (unsigned char)str[i + 3];
            if (n1 >= 0x90 && n1 <= 0x9F && n2 >= 0x80 && n2 <= 0xBF && n3 >= 0x80 && n3 <= 0xBF)
                return true;
        }
    }
    return false;
}

static Reflection::ScopedScriptable ScopedScriptableFor(lua_State* L)
{
    if (Reflection::scriptable_overrides.empty() || lua_type(L, 2) != LUA_TSTRING)
        return Reflection::ScopedScriptable(0, nullptr);

    return Reflection::ScopedScriptable(
        Reflection::instance_from_userdata(L, 1), lua_tostring(L, 2));
}

static int SafeCallOriginal(lua_CFunction original, lua_State* L, const char* name)
{
    if (!original)
        return 0;

    unsigned long Fault = 0;
    int Results = SafeCall::Invoke(original, L, &Fault);
    if (Fault != 0)
    {
        if (Fault == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        return 0;
    }
    return Results;
}

static void LogHookGate(lua_State* L, const char* Hook, const char* Key)
{
    static int LastState = -1;
    const bool HasUserdata = L->userdata != nullptr;
    const bool CapsOk = HasUserdata && (L->userdata->capabilities & 0xFFFFFFFFFFFFFF00ULL) == 0xFFFFFFFFFFFFFF00ULL;
    const bool SrcExpired = HasUserdata && L->userdata->source.expired();
    const int State = (HasUserdata ? 4 : 0) | (CapsOk ? 2 : 0) | (SrcExpired ? 1 : 0);
    if (State == LastState)
        return;
    LastState = State;

    SUPQ_LOG_ERR("%s gate REJECTED key '%s': userdata=%s caps=%s source.expired=%s",
        Hook, Key,
        HasUserdata ? "yes" : "NO",
        CapsOk ? "match" : "MISMATCH",
        SrcExpired ? "yes" : "NO");
    if (HasUserdata)
        SUPQ_LOG_ERR("   caps=0x%llX expected=0x%llX  identity=%u  extraspace=0x%llX",
            (unsigned long long)L->userdata->capabilities,
            (unsigned long long)MaxCapabilities,
            (unsigned)L->userdata->identity,
            (unsigned long long)L->userdata);
}

static int IndexHook_Impl(lua_State* L)
{
    if (!L || SharedVariables::LastDataModel == 0)
        return SafeCallOriginal(OriginalIndex, L, "__index");

    const Reflection::ScopedScriptable scriptable = ScopedScriptableFor(L);

    if (IsExecutorThread(L))
    {
        std::string Key = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";

        if (ContainsEmoji(Key))
        {
            luaL_error(L, "Blocked: vulnerable character detected");
            return 0;
        }

        if (const int Purchase = PurchaseGuard::Find(Key.c_str()); Purchase >= 0)
        {
            if (PurchaseGuard::ConsumeBypass(L, Purchase))
                return SafeCallOriginal(OriginalIndex, L, "__index");
            return PurchaseGuard::PushWrapper(L, Purchase);
        }

        for (const char* Function : UnsafeFunction)
        {
            if (Key == Function)
            {
                luaL_error(L, "Function '%s' has been blocked for security reasons", Function);
                return 0;
            }
        }

        if (Key == "HttpGet" || Key == "HttpGetAsync")
        {
            lua_pushcclosure(L, Http::HttpGet, nullptr, NULL);
            return 1;
        }
        else if (Key == "HttpPost" || Key == "HttpPostAsync")
        {
            lua_pushcclosure(L, Http::HttpPost, nullptr, NULL);
            return 1;
        }
        else if (Key == "GetObjects")
        {
            lua_pushcclosure(L, Miscellaneous::GetObjects, nullptr, NULL);
            return 1;
        }
    }
    else
    {
        if (lua_isstring(L, 2))
        {
            const char* prop = lua_tostring(L, 2);
            if (prop && (std::strcmp(prop, "Kick") == 0 || std::strcmp(prop, "kick") == 0))
            {
                lua_pushcclosure(L, [](lua_State*) -> int { return 0; }, nullptr, 0);
                return 1;
            }
        }
    }

    {
        const char* K = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
        if (K && (strcmp(K, "HttpGet") == 0 || strcmp(K, "HttpGetAsync") == 0 ||
                  strcmp(K, "HttpPost") == 0 || strcmp(K, "HttpPostAsync") == 0 ||
                  strcmp(K, "GetObjects") == 0))
            LogHookGate(L, "__index", K);
    }
    return SafeCallOriginal(OriginalIndex, L, "__index");
}

int IndexHook(lua_State* L)
{
    if (!L || SharedVariables::LastDataModel == 0)
    {
        if (OriginalIndex)
            return SafeCallOriginal(OriginalIndex, L, "__index");
        return 0;
    }

    const int depth = static_cast<int>(L->nCcalls);
    CallTrace::Enter("__index", lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr, depth);

    unsigned long Fault = 0;
    int Results = SafeCall::Invoke(IndexHook_Impl, L, &Fault);
    CallTrace::Leave("__index", depth);
    if (Fault != 0)
    {
        if (Fault == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        return 0;
    }
    return Results;
}

static const char* GetNamecallStringSafe(lua_State* L)
{
    __try {
        if (L && L->namecall && L->namecall->data)
            return L->namecall->data;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

static int NamecallHook_Impl(lua_State* L)
{
    if (!L)
        return 0;

    if (SharedVariables::LastDataModel == 0)
        return SafeCallOriginal(OriginalNamecall, L, "__namecall");

    if (IsExecutorThread(L))
    {
        const char* Key = GetNamecallStringSafe(L);

        if (Key)
        {
            if (ContainsEmoji(Key))
            {
                luaL_error(L, "Blocked: vulnerable character detected");
                return 0;
            }

            if (const int Purchase = PurchaseGuard::Find(Key); Purchase >= 0)
                return PurchaseGuard::Intercept(L, Purchase);

            for (const char* Function : UnsafeFunction)
            {
                if (std::strcmp(Key, Function) == 0)
                {
                    luaL_error(L, "Function '%s' has been blocked for security reasons", Function);
                    return 0;
                }
            }

            if (std::strcmp(Key, "HttpGet") == 0 || std::strcmp(Key, "HttpGetAsync") == 0)
            {
                return Http::HttpGet(L);
            }
            else if (std::strcmp(Key, "HttpPost") == 0 || std::strcmp(Key, "HttpPostAsync") == 0)
            {
                return Http::HttpPost(L);
            }
            else if (std::strcmp(Key, "GetObjects") == 0)
            {
                return Miscellaneous::GetObjects(L);
            }
        }
    }

    const char* Method = GetNamecallStringSafe(L);

    if (Method)
    {
        if ((std::strcmp(Method, "Kick") == 0 || std::strcmp(Method, "kick") == 0) && !IsExecutorThread(L))
        {
            SUPQ_LOG("Blocked client-side LocalPlayer:Kick from game script: %s", lua_isstring(L, 2) ? lua_tostring(L, 2) : "unknown");
            return 0;
        }

        if (strcmp(Method, "HttpGet") == 0 || strcmp(Method, "HttpGetAsync") == 0 ||
            strcmp(Method, "HttpPost") == 0 || strcmp(Method, "HttpPostAsync") == 0 ||
            strcmp(Method, "GetObjects") == 0)
            LogHookGate(L, "__namecall", Method);

        if (std::strcmp(Method, "BindToRenderStep") == 0)
            Signals::RecordRenderStepBinding(L);
        else if (std::strcmp(Method, "UnbindFromRenderStep") == 0)
            Signals::ForgetRenderStepBindingByArg(L);
    }

    return SafeCallOriginal(OriginalNamecall, L, "__namecall");
}

int NamecallHook(lua_State* L)
{
    if (!L || SharedVariables::LastDataModel == 0)
    {
        if (OriginalNamecall)
            return SafeCallOriginal(OriginalNamecall, L, "__namecall");
        return 0;
    }

    const int depth = static_cast<int>(L->nCcalls);
    CallTrace::Enter("__namecall",
        (L->namecall && L->namecall->data) ? L->namecall->data : nullptr, depth);

    unsigned long Fault = 0;
    int Results = SafeCall::Invoke(NamecallHook_Impl, L, &Fault);
    CallTrace::Leave("__namecall", depth);
    if (Fault != 0)
    {
        if (Fault == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        return 0;
    }
    return Results;
}

static int NewindexHook_Impl(lua_State* L)
{
    if (!L || SharedVariables::LastDataModel == 0)
        return SafeCallOriginal(OriginalNewindex, L, "__newindex");

    const Reflection::ScopedScriptable scriptable = ScopedScriptableFor(L);

    if (lua_type(L, 1) == LUA_TUSERDATA && lua_type(L, 2) == LUA_TSTRING)
    {
        const char* prop = lua_tostring(L, 2);
        if (prop && (strcmp(prop, "OnInvoke") == 0 || strcmp(prop, "OnClientInvoke") == 0 || strcmp(prop, "OnServerInvoke") == 0 || strcmp(prop, "OnClose") == 0))
        {
            Reflection::TrackCallbackValue(L, 1, 2, 3);

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

            lua_settop(L, top);
        }
    }

    return SafeCallOriginal(OriginalNewindex, L, "__newindex");
}

int NewindexHook(lua_State* L)
{
    if (!L || SharedVariables::LastDataModel == 0)
    {
        if (OriginalNewindex)
            return SafeCallOriginal(OriginalNewindex, L, "__newindex");
        return 0;
    }

    const int depth = static_cast<int>(L->nCcalls);
    CallTrace::Enter("__newindex", lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr, depth);

    unsigned long Fault = 0;
    int Results = SafeCall::Invoke(NewindexHook_Impl, L, &Fault);
    CallTrace::Leave("__newindex", depth);
    if (Fault != 0)
    {
        if (Fault == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        return 0;
    }
    return Results;
}

void InitializeHooks(lua_State* L)
{
    int OriginalTop = lua_gettop(L);

    lua_getglobal(L, "game");
    luaL_getmetafield(L, -1, "__index");
    if (lua_type(L, -1) == LUA_TFUNCTION || lua_type(L, -1) == LUA_TLIGHTUSERDATA)
    {
        GameIndexClosure = clvalue(luaA_toobject(L, -1));
        if (GameIndexClosure->c.f != IndexHook)
            OriginalIndex = GameIndexClosure->c.f;
        GameIndexClosure->c.f = IndexHook;
    }
    lua_pop(L, 1);

    luaL_getmetafield(L, -1, "__newindex");
    if (lua_type(L, -1) == LUA_TFUNCTION || lua_type(L, -1) == LUA_TLIGHTUSERDATA)
    {
        GameNewindexClosure = clvalue(luaA_toobject(L, -1));
        if (GameNewindexClosure->c.f != NewindexHook)
            OriginalNewindex = GameNewindexClosure->c.f;
        GameNewindexClosure->c.f = NewindexHook;
    }
    lua_pop(L, 1);

    luaL_getmetafield(L, -1, "__namecall");
    if (lua_type(L, -1) == LUA_TFUNCTION || lua_type(L, -1) == LUA_TLIGHTUSERDATA)
    {
        GameNamecallClosure = clvalue(luaA_toobject(L, -1));
        if (GameNamecallClosure->c.f != NamecallHook)
            OriginalNamecall = GameNamecallClosure->c.f;
        GameNamecallClosure->c.f = NamecallHook;
    }
    lua_pop(L, 1);

    lua_settop(L, OriginalTop);
}

static bool IsVmLuauFlag(const char* Name)
{
    static const char* const VmFlags[] = {
        "DebugLuauUserDefinedClassesRuntime", "FixMathNoisePrecision", "LuauBufferCage", "LuauCallLuauTm",
        "LuauCoroutineFinally", "LuauCostModel", "LuauEasyStateInit", "LuauFastpcall", "LuauFastpcallInterrupt",
        "LuauFrozenMetaButterfly", "LuauGcHeapShrinkFix", "LuauGcTraceUdata", "LuauInlineHitsThreshold",
        "LuauIntegerLibrary", "LuauLoadRemapOptionalUserdata", "LuauNewPointerEncode", "LuauOptimizeStringGsub",
        "LuauOptimizeStringSplit", "LuauPcallErrorContinuationReentry", "LuauPcallOptimize", "LuauPromoteProto",
        "LuauSandboxFreezesVectorMetatable", "LuauSplitTableLookups", "LuauTableArrayShrinkOrder",
        "LuauTableFindImprovement", "LuauTableRobustOom",
    };

    for (const char* VmFlag : VmFlags)
        if (std::strcmp(Name, VmFlag) == 0)
            return true;

    return false;
}

void ApplyLuauFlags()
{
    static const char* const Critical[] = {
        "LuauFrozenMetaButterfly", "LuauNewPointerEncode", "LuauBufferCage", "LuauCoroutineFinally",
    };

    int Mirrored = 0, Missing = 0;
    std::string Value;

    for (Luau::FValue<bool>* Flag = Luau::FValue<bool>::list; Flag; Flag = Flag->next)
    {
        if (!IsVmLuauFlag(Flag->name))
            continue;

        if (!Miscellaneous::GetFlag(Flag->name, Value))
        {
            ++Missing;
            for (const char* Name : Critical)
                if (std::strcmp(Flag->name, Name) == 0)
                    SUPQ_LOG_ERR("  luau flags: client has no %s, keeping %d - VM copy may disagree with the client", Name, int(Flag->value));
            continue;
        }

        Flag->value = _stricmp(Value.c_str(), "true") == 0;
        ++Mirrored;
    }

    for (Luau::FValue<int>* Flag = Luau::FValue<int>::list; Flag; Flag = Flag->next)
    {
        if (!IsVmLuauFlag(Flag->name))
            continue;

        if (!Miscellaneous::GetFlag(Flag->name, Value))
        {
            ++Missing;
            continue;
        }

        int Parsed = 0;
        const auto Result = std::from_chars(Value.data(), Value.data() + Value.size(), Parsed);
        if (Result.ec == std::errc{} && Result.ptr == Value.data() + Value.size())
        {
            Flag->value = Parsed;
            ++Mirrored;
        }
    }

    SUPQ_LOG_OK("  luau flags: mirrored %d from client, %d not present", Mirrored, Missing);
}

void Environment::SetupEnvironment(lua_State* L)
{
    if (CloneGuard::Detected() || CloneGuard::RivalProcessRunning() || CloneGuard::RivalPortsListening())
    {
        SUPQ_LOG_ERR("blacklisted repack detected - aborting setup and crashing");
        CloneGuard::CrashClient();
        return;
    }

    SUPQ_LOG_OK("  env: calling ApplyLuauFlags");
    ApplyLuauFlags();

    for (auto& entry : L->global->udatadirect)
    {
        setnilvalue(&entry.indextm);
        setnilvalue(&entry.newindextm);
        setnilvalue(&entry.namecalltm);
    }
    Closures::ResetLibraryState();
    Signals::ResetLibraryState();

    SharedVariables::ExecutorClosures.clear();
    SharedVariables::ExecutorFastcallsDisabled = false;
    Environment::function_array.clear();
    {
        std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
        std::queue<std::function<void()>> empty;
        SharedVariables::YieldQueue.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(SharedVariables::ExecutionRequestsMutex);
        SharedVariables::ExecutionRequests.clear();
    }

    luaL_sandboxthread(L);

    Http::RegisterLibrary(L);
    Crypt::RegisterLibrary(L);
    Input::RegisterLibrary(L);
    Debug::RegisterLibrary(L);
    Cache::RegisterLibrary(L);
    Signals::RegisterLibrary(L);
    LuaSignal::RegisterLibrary(L);
    Script::RegisterLibrary(L);
    Closures::RegisterLibrary(L);
    Trampoline::RegisterLibrary(L);
    Instance::RegisterLibrary(L);
    Metatable::RegisterLibrary(L);
    Filesystem::RegisterLibrary(L);
    Reflection::RegisterLibrary(L);
    Miscellaneous::RegisterLibrary(L);
    Regex::RegisterLibrary(L);
    FileDialog::RegisterLibrary(L);
    Oth::RegisterLibrary(L);
    Drawing::RegisterLibrary(L);
    ImGuiLib::RegisterLibrary(L);
    Console::RegisterLibrary(L);
    supQLib::RegisterLibrary(L);
    Bit::RegisterLibrary(L);
    Websocket::RegisterLibrary(L);
    Actors::RegisterLibrary(L);
    Actors::RegisterGameEnvironment(L);
    Raknet::RegisterLibrary(L);
    Compat::RegisterLibrary(L);

    SUPQ_LOG_OK("  env: all libraries registered, calling InitializeHooks");
    InitializeHooks(L);
    SUPQ_LOG_OK("  env: InitializeHooks ok");
    luaL_sandboxthread(L);
    SUPQ_LOG_OK("  env: luaL_sandboxthread #2 ok");
    Closures::HardenErrorFrameLibs(L);
    SUPQ_LOG_OK("  env: HardenErrorFrameLibs ok");

    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setglobal(L, "_G");

    lua_newtable(L);
    lua_setglobal(L, "shared");
    SUPQ_LOG_OK("  env: globals (_G, shared) ok");

    static bool exploitHelperExecuted = false;
    if (!exploitHelperExecuted) {
        exploitHelperExecuted = true;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    TaskScheduler::RequestExecution(ExecutionGuard::Wrap(LuaScripts::Environment));
    TaskScheduler::RequestExecution(ExecutionGuard::Wrap(LuaScripts::Compat));
    TaskScheduler::RequestExecution(ExecutionGuard::Wrap(LuaScripts::Decompile));
    TaskScheduler::RequestExecution(ExecutionGuard::Wrap(LuaScripts::SaveInstance));

    Renderer::Initialize();

    if (CloneGuard::Detected())
    {
        SUPQ_LOG_ERR("blacklisted repack detected - aborting startup");
        CloneGuard::CrashClient();
        return;
    }

    {
        ImGuiToast toast(ImGuiToastType::Success, 5000);
        toast.setTitle(BrandGuard::ToastTitle().c_str());
        toast.setContent(BrandGuard::ToastUrl().c_str());
        Renderer::PushToast(toast);
    }
    SUPQ_LOG_OK("  env: startup scripts queued - SetupEnvironment complete");
}

void Environment::TeardownEnvironment()
{
    GameIndexClosure = nullptr;
    GameNewindexClosure = nullptr;
    GameNamecallClosure = nullptr;

    Closures::ResetLibraryState();
    Signals::ResetLibraryState();
    Reflection::ResetLibraryState();
    Drawing::ResetLibraryState();

    SharedVariables::ExecutorClosures.clear();
    SharedVariables::ClearHiddenClosures();
    SharedVariables::ExecutorFastcallsDisabled = false;
    Environment::function_array.clear();

    {
        std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
        std::queue<std::function<void()>> empty;
        SharedVariables::YieldQueue.swap(empty);
    }
}
