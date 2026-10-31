#include <cstring>
#include <mutex>

#include <Runtime/Execution/ExecutionGuard.hpp>
#include <Core/DebugLog.hpp>
#include <Core/Context.hpp>
#include <Runtime/Execution/ScriptExecution.hpp>
#include <Scripting/Bridge/SafeCall.hpp>
#include <Network/Raknet/Raknet.hpp>
#include <Scripting/Environment.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Coroutines/Yielding.hpp>
#include <Scripting/Modules/Websocket.hpp>
#include <Scripting/Obfuscation.hpp>
#include <Runtime/Execution/AutoExecute.hpp>
#include <Ui/Overlay/Renderer.hpp>
#include <Security/CloneGuard.hpp>

namespace
{
	[[noreturn]] void q7Vn3a() noexcept
	{
		TerminateProcess(GetCurrentProcess(), 0);
		for (;;) {}
	}

	bool m4Kx8p(const std::string& source) noexcept
	{
		return source.find(SUPQ_OBF("Quorum")) != std::string::npos ||
		       source.find(SUPQ_OBF("_CrescentConsoleHook")) != std::string::npos ||
		       source.find(SUPQ_OBF("_CrescentGameHubProbe")) != std::string::npos;
	}

	bool Readable(uintptr_t Address, size_t Size) noexcept
	{
		if (Address < 0x10000 || Address > 0x7FFFFFFEFFFF || Address + Size < Address)
			return false;

		MEMORY_BASIC_INFORMATION Info{};
		if (!VirtualQuery(reinterpret_cast<const void*>(Address), &Info, sizeof(Info)))
			return false;
		if (Info.State != MEM_COMMIT || (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;

		return Address + Size <= reinterpret_cast<uintptr_t>(Info.BaseAddress) + Info.RegionSize;
	}

	uintptr_t SafeReadPtr(uintptr_t Address) noexcept
	{
		if (!Readable(Address, sizeof(uintptr_t)))
			return 0;
		__try { return *reinterpret_cast<uintptr_t*>(Address); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	bool StringEquals(uintptr_t String, const char* Want) noexcept
	{
		if (!Readable(String, 0x20))
			return false;

		__try
		{
			const size_t Size = *reinterpret_cast<size_t*>(String + 0x10);
			const size_t Capacity = *reinterpret_cast<size_t*>(String + 0x18);
			if (Size != std::strlen(Want) || Capacity < Size)
				return false;

			const uintptr_t Data = Capacity >= 0x10 ? *reinterpret_cast<uintptr_t*>(String) : String;
			return Readable(Data, Size) && std::memcmp(reinterpret_cast<const void*>(Data), Want, Size) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool InstanceIsClass(uintptr_t Instance, const char* Want) noexcept
	{
		const uintptr_t ClassDescriptor = SafeReadPtr(Instance + Offsets::Instance::ClassDescriptor);
		return ClassDescriptor && StringEquals(SafeReadPtr(ClassDescriptor + Offsets::Instance::ClassName), Want);
	}

	bool ReadClassName(uintptr_t Instance, char* Out, size_t OutSize) noexcept
	{
		Out[0] = '\0';
		const uintptr_t ClassDescriptor = SafeReadPtr(Instance + Offsets::Instance::ClassDescriptor);
		const uintptr_t String = ClassDescriptor ? SafeReadPtr(ClassDescriptor + Offsets::Instance::ClassName) : 0;
		if (!Readable(String, 0x20))
			return false;

		__try
		{
			const size_t Size = *reinterpret_cast<size_t*>(String + 0x10);
			const size_t Capacity = *reinterpret_cast<size_t*>(String + 0x18);
			if (!Size || Size >= OutSize || Capacity < Size)
				return false;

			const uintptr_t Data = Capacity >= 0x10 ? *reinterpret_cast<uintptr_t*>(String) : String;
			if (!Readable(Data, Size))
				return false;
			std::memcpy(Out, reinterpret_cast<const void*>(Data), Size);
			Out[Size] = '\0';
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Out[0] = '\0';
			return false;
		}
	}

	std::string LastSchedulerError;

	void ReportSchedulerError(const char* Message)
	{
		if (LastSchedulerError == Message)
			return;

		LastSchedulerError = Message;
		SUPQ_LOG_ERR("scheduler: %s", Message);
	}
}

void TaskScheduler::SetProtoCapabilities(Proto* root, uintptr_t* caps)
{
	if (!root) return;

	std::vector<Proto*> stack{ root };
	while (!stack.empty())
	{
		Proto* p = stack.back(); stack.pop_back();
		if (!p) continue;

		p->userdata = caps;

		if (p->sizep > 0 && p->sizep <= 100000 && p->p)
			for (int i = 0; i < p->sizep; ++i)
				if (p->p[i]) stack.push_back(p->p[i]);
	}
}

void TaskScheduler::SetThreadCapabilities(lua_State* L, int Level, uintptr_t Capabilities)
{
	if (!L || !L->userdata)
		return;

	L->userdata->identity = Level;
	L->userdata->capabilities = Capabilities;
}

namespace
{
	void SetInstanceIdentity(int Identity) noexcept
	{
		__try
		{
			const uintptr_t IdentityPointer = *reinterpret_cast<uintptr_t*>(Offsets::RobloxThread::IdentityPtr);
			if (!IdentityPointer || IdentityPointer > 0xFFFFFFFFFFFF)
				return;

			const uintptr_t IdentityStructure = Roblox::GetIdentityStruct(IdentityPointer);
			if (!IdentityStructure)
				return;

			*reinterpret_cast<int32_t*>(IdentityStructure) = Identity;
			*reinterpret_cast<uintptr_t*>(IdentityStructure + Offsets::Capabilities::Capabilities) = Roblox::GetCapabilities(&Identity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
}

void TaskScheduler::SetIdentity(lua_State* Thread, int Identity, bool IsInstance)
{
	if (!Thread) {
		return;
	}
	if (!Thread->userdata) {
		return;
	}

	rbxextraspace* bdata = reinterpret_cast<rbxextraspace*>(Thread->userdata);
	bdata->identity = Identity;
	bdata->capabilities = Roblox::GetCapabilities(&Identity);

	if (IsInstance)
		SetInstanceIdentity(Identity);
}

uintptr_t TaskScheduler::GetDataModel()
{
	uintptr_t FakeDataModel = *reinterpret_cast<uintptr_t*>(Offsets::DataModel::FakeDataModelPointer);
	if (!FakeDataModel)
		return 0;

	uintptr_t DataModel = *reinterpret_cast<uintptr_t*>(FakeDataModel + Offsets::DataModel::FakeDataModelToDataModel);

	if (!DataModel) return 0;

	return DataModel;
}

namespace
{
	std::mutex ScriptContextCacheMutex;
	uintptr_t CachedDataModel = 0;
	uintptr_t CachedChildrenOffset = 0;
	uintptr_t CachedEntryOffset = 0;

	uintptr_t ReadCachedScriptContext(uintptr_t DataModel)
	{
		std::lock_guard<std::mutex> Lock(ScriptContextCacheMutex);
		if (DataModel != CachedDataModel)
			return 0;

		const uintptr_t Begin = SafeReadPtr(SafeReadPtr(DataModel + CachedChildrenOffset));
		const uintptr_t Child = Begin ? SafeReadPtr(Begin + CachedEntryOffset) : 0;
		if (!Child)
			CachedDataModel = 0;
		return Child;
	}

	void ForgetScriptContextSlot()
	{
		std::lock_guard<std::mutex> Lock(ScriptContextCacheMutex);
		CachedDataModel = 0;
	}

	void CacheScriptContextSlot(uintptr_t DataModel, uintptr_t ChildrenOffset, uintptr_t EntryOffset)
	{
		std::lock_guard<std::mutex> Lock(ScriptContextCacheMutex);
		CachedDataModel = DataModel;
		CachedChildrenOffset = ChildrenOffset;
		CachedEntryOffset = EntryOffset;
	}
}

uintptr_t TaskScheduler::GetScriptContext(uintptr_t DataModel)
{
	if (!DataModel) return 0;

	if (const uintptr_t Cached = ReadCachedScriptContext(DataModel))
		return Cached;

	const std::string WantedString = SUPQ_OBF("ScriptContext");
	const char* const Wanted = WantedString.c_str();
	const uintptr_t ChildrenOffset = Offsets::Instance::Children;
	constexpr uintptr_t EntryStride = 0x10;

	const uintptr_t Container = SafeReadPtr(DataModel + ChildrenOffset);
	const uintptr_t Begin = Container ? SafeReadPtr(Container + 0x0) : 0;
	const uintptr_t End = Container ? SafeReadPtr(Container + 0x8) : 0;
	if (!Begin || End <= Begin || (End - Begin) > 0x10000 || ((End - Begin) % EntryStride) != 0)
	{
		SUPQ_LOG_ERR("GetScriptContext: children+0x%llX -> container 0x%llX has no sane range (begin 0x%llX end 0x%llX)",
			(unsigned long long)ChildrenOffset, (unsigned long long)Container,
			(unsigned long long)Begin, (unsigned long long)End);
		return 0;
	}

	uintptr_t Match = 0;
	uintptr_t MatchEntry = 0;
	size_t Matches = 0;
	size_t Decoded = 0;
	for (uintptr_t Entry = Begin; Entry < End; Entry += EntryStride)
	{
		const uintptr_t Child = SafeReadPtr(Entry);
		if (!Child)
			continue;

		char ClassName[64];
		if (ReadClassName(Child, ClassName, sizeof(ClassName)))
			++Decoded;
		if (InstanceIsClass(Child, Wanted))
		{
			Match = Child;
			MatchEntry = Entry;
			++Matches;
		}
	}

	const size_t Count = (End - Begin) / EntryStride;
	if (Matches == 1)
	{
		SUPQ_LOG_OK("GetScriptContext: matched by class at child #%llu of %llu -> 0x%llX",
			(unsigned long long)((MatchEntry - Begin) / EntryStride), (unsigned long long)Count,
			(unsigned long long)Match);
		CacheScriptContextSlot(DataModel, ChildrenOffset, MatchEntry - Begin);
		return Match;
	}
	if (Matches > 1)
	{
		SUPQ_LOG_ERR("GetScriptContext: %llu children of class ScriptContext; refusing to pick one",
			(unsigned long long)Matches);
		return 0;
	}

	char LegacyClass[64] = "?";
	const uintptr_t LegacyEntry = Begin + Offsets::DataModel::ScriptContext;
	const uintptr_t Legacy = LegacyEntry < End ? SafeReadPtr(LegacyEntry) : 0;
	if (Legacy)
		ReadClassName(Legacy, LegacyClass, sizeof(LegacyClass));

	SUPQ_LOG_ERR("GetScriptContext: %llu children, %llu class names decoded, none is ScriptContext; child #%llu is \"%s\"",
		(unsigned long long)Count, (unsigned long long)Decoded,
		(unsigned long long)(Offsets::DataModel::ScriptContext / EntryStride), LegacyClass);

	for (uintptr_t Entry = Begin; Entry < End; Entry += EntryStride)
	{
		char ClassName[64];
		const uintptr_t Child = SafeReadPtr(Entry);
		if (Child && ReadClassName(Child, ClassName, sizeof(ClassName)) && std::strstr(ClassName, "Script"))
			SUPQ_LOG("GetScriptContext:   child #%llu \"%s\" 0x%llX",
				(unsigned long long)((Entry - Begin) / EntryStride), ClassName, (unsigned long long)Child);
	}

	if (Decoded == 0 && Legacy)
	{
		SUPQ_LOG("GetScriptContext: no class names decoded; falling back to fixed index 0x%llX -> 0x%llX",
			(unsigned long long)Offsets::DataModel::ScriptContext, (unsigned long long)Legacy);
		CacheScriptContextSlot(DataModel, ChildrenOffset, Offsets::DataModel::ScriptContext);
		return Legacy;
	}

	return 0;
}

namespace
{
	bool ValidState(uintptr_t Address, uintptr_t ScriptContext) noexcept
	{
		if ((Address & 0x7) != 0 || !Readable(Address, sizeof(lua_State)))
			return false;

		__try
		{
			auto* State = reinterpret_cast<lua_State*>(Address);
			if (State->tt != LUA_TTHREAD)
				return false;

			const uintptr_t Stack = reinterpret_cast<uintptr_t>(State->stack);
			const uintptr_t Base = reinterpret_cast<uintptr_t>(State->base);
			const uintptr_t Top = reinterpret_cast<uintptr_t>(State->top);
			const uintptr_t StackLast = reinterpret_cast<uintptr_t>(State->stack_last);

			if (!Readable(Stack, sizeof(TValue)) || Base < Stack || Top < Base || StackLast < Top)
				return false;

			if (State->stacksize < LUA_MINSTACK || State->stacksize > 0x100000)
				return false;

			const uintptr_t BaseCi = reinterpret_cast<uintptr_t>(State->base_ci);
			const uintptr_t Ci = reinterpret_cast<uintptr_t>(State->ci);
			const uintptr_t EndCi = reinterpret_cast<uintptr_t>(State->end_ci);

			if (!Readable(BaseCi, sizeof(CallInfo)) || Ci < BaseCi || EndCi < Ci)
				return false;

			if ((Ci - BaseCi) % sizeof(CallInfo) != 0)
				return false;

			if (State->size_ci <= 0 || State->size_ci > 0x10000)
				return false;

			const uintptr_t Extra = reinterpret_cast<uintptr_t>(State->userdata);
			if (!Readable(Extra, sizeof(RobloxExtraSpace)))
				return false;

			auto* Space = reinterpret_cast<RobloxExtraSpace*>(Extra);
			const uintptr_t IdentityCtx = reinterpret_cast<uintptr_t>(Space->identity_script_context);
			const uintptr_t SharedPtr = reinterpret_cast<uintptr_t>(Space->shared.get());
			const uintptr_t SharedCtx = (SharedPtr && Readable(SharedPtr, sizeof(RobloxExtraSpace::Shared)))
				? reinterpret_cast<uintptr_t>(Space->shared->script_context)
				: 0;

			if (IdentityCtx != ScriptContext && SharedCtx != ScriptContext)
				return false;

			const uintptr_t Global = reinterpret_cast<uintptr_t>(State->global);
			if (!Readable(Global, sizeof(void*) * 8))
				return false;

			const uintptr_t MainThread = reinterpret_cast<uintptr_t>(State->global->mainthread);
			if (!Readable(MainThread, sizeof(lua_State)) || State->global->mainthread->tt != LUA_TTHREAD)
				return false;

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	lua_State* KiGetLuaState(uintptr_t ScriptContext) noexcept
	{
		if (!Readable(ScriptContext, 0x1000))
			return nullptr;

		for (size_t Offset = 0; Offset <= 0x1000 - sizeof(uintptr_t); Offset += sizeof(uint32_t))
		{
			const uintptr_t Slot = ScriptContext + Offset;
			if (!Readable(Slot, sizeof(uintptr_t)))
				continue;

			uintptr_t Direct = 0;
			uint32_t Lo = 0;
			uint32_t Hi = 0;
			__try
			{
				Direct = *reinterpret_cast<const uintptr_t*>(Slot);
				Lo = *reinterpret_cast<const uint32_t*>(Slot);
				Hi = *reinterpret_cast<const uint32_t*>(Slot + sizeof(uint32_t));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				continue;
			}

			if ((Offset & 0x7) == 0 && ValidState(Direct, ScriptContext))
				return reinterpret_cast<lua_State*>(Direct);

			const uint32_t KeyLo = static_cast<uint32_t>(Slot);
			const uint32_t KeyHi = static_cast<uint32_t>(Slot + sizeof(uint32_t));

			const uintptr_t Candidates[] = {
				(static_cast<uintptr_t>(Hi + KeyLo) << 32) | static_cast<uint32_t>(Lo + KeyLo),
				(static_cast<uintptr_t>(Hi - KeyLo) << 32) | static_cast<uint32_t>(Lo - KeyLo),
				(static_cast<uintptr_t>(KeyHi - Hi) << 32) | static_cast<uint32_t>(KeyLo - Lo),
				(static_cast<uintptr_t>(KeyLo - Hi) << 32) | static_cast<uint32_t>(KeyLo - Lo),
				(static_cast<uintptr_t>(Hi + KeyHi) << 32) | static_cast<uint32_t>(Lo + KeyLo),
				(static_cast<uintptr_t>(Hi - KeyHi) << 32) | static_cast<uint32_t>(Lo - KeyLo),
				(static_cast<uintptr_t>(Hi ^ KeyLo) << 32) | static_cast<uint32_t>(Lo ^ KeyLo),
				(static_cast<uintptr_t>(Hi ^ KeyHi) << 32) | static_cast<uint32_t>(Lo ^ KeyLo),
			};

			for (uintptr_t Candidate : Candidates)
			{
				if (ValidState(Candidate, ScriptContext))
					return reinterpret_cast<lua_State*>(Candidate);
			}
		}

		return nullptr;
	}
}

lua_State* TaskScheduler::GetLuaStateForInstance(uintptr_t Instance)
{
	if (!Instance)
		return nullptr;

	__try
	{
		*reinterpret_cast<BOOLEAN*>(Instance + Offsets::ExtraSpace::RequireBypass) = 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	lua_State* state = nullptr;
	__try
	{
		uint64_t a2 = 0;
		uint64_t a3 = 0;
		state = Roblox::GetLuaStateForInstance(Instance, &a2, &a3);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		SUPQ_LOG_ERR("Roblox::GetLuaStateForInstance crashed via SEH");
	}

	if (!state)
	{
		__try
		{
			const uintptr_t Slot = Instance + Offsets::Luau::ScriptContextToLuaState;
			const uint32_t Key = static_cast<uint32_t>(Slot);
			const uint32_t Lo = *reinterpret_cast<const uint32_t*>(Slot) + Key;
			const uint32_t Hi = *reinterpret_cast<const uint32_t*>(Slot + 4) + Key;
			state = reinterpret_cast<lua_State*>((static_cast<uintptr_t>(Hi) << 32) | Lo);
			if (state)
			{
				SUPQ_LOG("Fell back to ScriptContextToLuaState (0x%llX) -> 0x%p",
					(unsigned long long)Offsets::Luau::ScriptContextToLuaState, (void*)state);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			SUPQ_LOG_ERR("ScriptContextToLuaState fallback crashed via SEH");
		}
	}

	if (!state)
	{
		if (lua_State* scanned = KiGetLuaState(Instance))
		{
			state = scanned;
			SUPQ_LOG("Fell back to KiGetLuaState dynamic scanner -> 0x%p", (void*)state);
		}
	}

	return state;
}

lua_State* TaskScheduler::GetLuaState(uintptr_t ScriptContext, uintptr_t Identity, uintptr_t Script)
{
	return Roblox::GetLuaStateForInstance(ScriptContext, &Identity, &Script);
}

namespace
{
	uintptr_t GetJobByTypeName(const std::string& TypeName)
	{
		uintptr_t Scheduler =
			*reinterpret_cast<uintptr_t*>(Offsets::Scheduler::TaskScheduler);
		if (!Scheduler)
			return 0;

		uintptr_t JobsStart =
			*reinterpret_cast<uintptr_t*>(Scheduler + Offsets::Scheduler::JobsStart);
		uintptr_t JobsEnd =
			*reinterpret_cast<uintptr_t*>(Scheduler + Offsets::Scheduler::JobsEnd);

		if (!JobsStart || !JobsEnd || JobsStart >= JobsEnd)
			return 0;

		const size_t nameLen = TypeName.size();
		if (nameLen == 0 || nameLen >= 0x100)
			return 0;

		static constexpr uintptr_t kNameOffsets[] = { 0x18, 0xF8, 0x118 };

		for (uintptr_t Current = JobsStart; Current < JobsEnd; Current += 0x10)
		{
			uintptr_t Job = *reinterpret_cast<uintptr_t*>(Current);
			if (!Job)
				continue;

			for (uintptr_t off : kNameOffsets)
			{
				if (!Readable(Job + off, sizeof(uintptr_t)))
					continue;

				const char* candidate = *reinterpret_cast<const char**>(Job + off);
				if (!candidate)
					continue;

				uintptr_t candAddr = reinterpret_cast<uintptr_t>(candidate);
				if (candAddr < 0x10000 || candAddr > 0x7FFFFFFFFFFF)
					continue;

				if (!Readable(candAddr, nameLen))
					continue;

				if (memcmp(candidate, TypeName.c_str(), nameLen) == 0)
					return Job;
			}
		}

		return 0;
	}

	uintptr_t __fastcall stepHook(uintptr_t Job, uintptr_t Stats)
	{
		if (!SharedVariables::OriginalStepFunction)
			return 0;

		if (SharedVariables::JobStepLocked.load(std::memory_order_acquire))
			return SharedVariables::OriginalStepFunction(Job, Stats);

		if (SharedVariables::ExploitThread)
		{
			for (;;)
			{
				std::string Script;
				{
					std::lock_guard<std::mutex> lock(SharedVariables::ExecutionRequestsMutex);
					if (SharedVariables::ExecutionRequests.empty())
						break;
					Script = std::move(SharedVariables::ExecutionRequests.front());
					SharedVariables::ExecutionRequests.erase(
						SharedVariables::ExecutionRequests.begin());
				}
				if (!Script.empty())
				{
					unsigned long Fault = 0;
					auto ExecCtx = std::make_pair(SharedVariables::ExploitThread, &Script);
					SafeCall::InvokeVoid([](void* p) {
						auto* ctx = static_cast<std::pair<lua_State*, std::string*>*>(p);
						Execution::ExecuteBytecode(ctx->first, *ctx->second);
						}, &ExecCtx, &Fault);
				}
			}

			unsigned long RakFault = 0;
			SafeCall::InvokeVoid([](void* p) {
				Raknet::ProcessCallbacks(static_cast<lua_State*>(p));
				}, SharedVariables::ExploitThread, &RakFault);

			unsigned long YieldFault = 0;
			SafeCall::InvokeVoid([](void*) {
				Yielding::RunYield();
				}, nullptr, &YieldFault);
		}

		return SharedVariables::OriginalStepFunction(Job, Stats);
	}

	bool InstallWHSJHook()
	{
		uintptr_t GameWHSJ = GetJobByTypeName(SUPQ2_OBF("WaitingHybridScriptsJob(Default;Ugc)"));
		if (!GameWHSJ || !Readable(GameWHSJ, sizeof(uintptr_t)))
		{
			SUPQ_LOG_ERR("InstallWHSJHook: WaitingHybridScriptsJob not found");
			return false;
		}

		SharedVariables::LastWaitingHybridScriptsJob = GameWHSJ;

		uintptr_t* JobVTable = *reinterpret_cast<uintptr_t**>(GameWHSJ);
		if (JobVTable != SharedVariables::HybridJobVtable)
		{
			if (!SharedVariables::HybridJobVtable)
				SharedVariables::HybridJobVtable = new uintptr_t[9];

			memcpy(SharedVariables::HybridJobVtable, JobVTable, sizeof(uintptr_t) * 9);

			SharedVariables::OriginalStepFunction =
				reinterpret_cast<SharedVariables::JobStep_t>(JobVTable[8]);
			SharedVariables::HybridJobVtable[8] =
				reinterpret_cast<uintptr_t>(&stepHook);
		}

		SharedVariables::JobStepLocked.store(false, std::memory_order_release);
		*reinterpret_cast<uintptr_t**>(SharedVariables::LastWaitingHybridScriptsJob) =
			SharedVariables::HybridJobVtable;

		SUPQ_LOG_OK("InstallWHSJHook: vtable[8] hooked on job 0x%llX",
			(unsigned long long)GameWHSJ);
		return true;
	}
}

namespace
{
	int ReadPlaceIdThunk(lua_State* L)
	{
		lua_getglobal(L, SUPQ2_OBF("game"));
		lua_getfield(L, -1, SUPQ2_OBF("PlaceId"));
		return 1;
	}

	int64_t ReadLuaPlaceId(lua_State* L)
	{
		if (!L)
			return -1;

		const int Top = lua_gettop(L);
		lua_pushcfunction(L, ReadPlaceIdThunk, nullptr);
		int64_t PlaceId = -1;
		if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1))
			PlaceId = static_cast<int64_t>(lua_tonumber(L, -1));
		lua_settop(L, Top);
		return PlaceId;
	}
}



void TaskScheduler::NotifySetupFailed(const char* Reason)
{
	Renderer::PushErrorToast(SUPQ2_OBF("Failed to Load"), Reason);
}

bool TaskScheduler::SetupExploit(uintptr_t DataModel)
{
	DataModel = TaskScheduler::GetDataModel();

	if (!DataModel) return 0;

	uintptr_t ScriptContext = TaskScheduler::GetScriptContext(DataModel);
	if (!ScriptContext) {
		Roblox::Print(3, SUPQ2_OBF("Failed to get ScriptContext, try joining a different game"));
		SUPQ_LOG_ERR("SetupExploit: GetScriptContext returned 0 (DataModel 0x%llX, ScriptContext offset 0x%llX)",
			(unsigned long long)DataModel, (unsigned long long)Offsets::DataModel::ScriptContext);
		return false;
	}

	SUPQ_LOG_OK("SetupExploit: ScriptContext 0x%llX", (unsigned long long)ScriptContext);

	lua_State* RobloxState = TaskScheduler::GetLuaStateForInstance(ScriptContext);
	if (!RobloxState) {
		SUPQ_LOG_ERR("SetupExploit: GetLuaStateForInstance returned null");
		return 0;
	}

	SUPQ_LOG_OK("SetupExploit: got lua_State 0x%llX", (unsigned long long)RobloxState);

	if (!Readable(reinterpret_cast<uintptr_t>(RobloxState), sizeof(lua_State))) {
		SUPQ_LOG_ERR("SetupExploit: lua_State 0x%llX is not readable memory - wrong ScriptContext?",
			(unsigned long long)RobloxState);
		ForgetScriptContextSlot();
		return false;
	}

	static int s_ExploitThreadRef = LUA_REFNIL;
	static global_State* s_ExploitThreadRefOwner = nullptr;
	if (s_ExploitThreadRef != LUA_REFNIL)
	{
		if (s_ExploitThreadRefOwner == RobloxState->global)
			lua_unref(RobloxState, s_ExploitThreadRef);
		else
			SUPQ_LOG("SetupExploit: dropping stale thread ref %d from previous state", s_ExploitThreadRef);
		s_ExploitThreadRef = LUA_REFNIL;
		s_ExploitThreadRefOwner = nullptr;
	}

	SharedVariables::ExploitThread = lua_newthread(RobloxState);
	if (!SharedVariables::ExploitThread) {
		SUPQ_LOG_ERR("SetupExploit: lua_newthread returned null");
		return 0;
	}
	s_ExploitThreadRef = lua_ref(RobloxState, -1);
	s_ExploitThreadRefOwner = RobloxState->global;
	lua_pop(RobloxState, 1);

	SUPQ_LOG_OK("SetupExploit: [1/4] lua_newthread -> 0x%llX (anchored ref %d)",
		(unsigned long long)SharedVariables::ExploitThread, s_ExploitThreadRef);

	SharedVariables::StateGeneration.fetch_add(1);

	TaskScheduler::SetThreadCapabilities(SharedVariables::ExploitThread, 8, MaxCapabilities);
	SUPQ_LOG_OK("SetupExploit: [2/4] SetThreadCapabilities ok");

	Environment::SetupEnvironment(SharedVariables::ExploitThread);
	SUPQ_LOG_OK("SetupExploit: [3/4] SetupEnvironment ok");

	SharedVariables::AttachedLuaPlaceId.store(ReadLuaPlaceId(SharedVariables::ExploitThread), std::memory_order_release);
	SharedVariables::ExecutorHeartbeatConnection.store(0, std::memory_order_release);

	if (!InstallWHSJHook())
	{
		SUPQ_LOG_ERR("SetupExploit: InstallWHSJHook failed");
		SharedVariables::ExploitThread = nullptr;
		return false;
	}
	SUPQ_LOG_OK("SetupExploit: WHSJ vtable[8] hook installed");

	SUPQ_LOG_OK("SetupExploit: [4/4] SetupExecution ok");

	LastSchedulerError.clear();

	AutoExecute::ExecuteFolder();
	return true;
}

void TaskScheduler::RequestExecution(std::string Script)
{
	if (CloneGuard::Detected())
	{
		CloneGuard::CrashClient();
		return;
	}

	if (!ExecutionGuard::Consume(Script))
	{
		SUPQ_LOG_ERR("execute: rejected (missing or invalid execution token)");
		return;
	}

	if (m4Kx8p(Script))
		q7Vn3a();

	size_t nonSpace = Script.find_first_not_of(" \t\r\n");
	if (nonSpace == std::string::npos)
		return;

	std::string Bytecode = Execution::PrepareScript(std::move(Script));
	if (Bytecode.empty())
		return;

	std::lock_guard<std::mutex> lock(SharedVariables::ExecutionRequestsMutex);
	SharedVariables::ExecutionRequests.push_back(std::move(Bytecode));
}
