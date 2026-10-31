#include <Core/Context.hpp>
#include <Scripting/Coroutines/Yielding.hpp>

void Yielding::RunYield()
{
	std::function<void()> YieldingRequest;
	{
		std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
		if (SharedVariables::YieldQueue.empty())
			return;
		YieldingRequest = std::move(SharedVariables::YieldQueue.front());
		SharedVariables::YieldQueue.pop();
	}
	YieldingRequest();
}

int Yielding::YieldExecution(lua_State* L, const std::function<Yielded()>& YieldingClosure)
{
	const uint64_t Generation = SharedVariables::StateGeneration.load();
	lua_pushthread(L);
	int YieldedThreadRef = lua_ref(L, -1);
	lua_pop(L, 1);

	std::thread([=]
	{
		Yielded ResumeFunction = YieldingClosure();
		if (Generation != SharedVariables::StateGeneration.load())
			return;

		{
			std::lock_guard<std::mutex> lock(SharedVariables::YieldQueueMutex);
			SharedVariables::YieldQueue.emplace([=]() -> void
			{
				if (Generation != SharedVariables::StateGeneration.load())
					return;

				if (lua_status(L) != LUA_YIELD)
				{
					lua_unref(L, YieldedThreadRef);
					return;
				}

				YieldingLuaThread YieldingLuaThreadObject = { .L = L };
				YieldingLuaThread* YieldingLuaThread = &YieldingLuaThreadObject;

				YieldState YieldState = { 0 };
				Roblox::ScriptContextResume((uintptr_t)L->userdata->shared->script_context + Offsets::ExtraSpace::ScriptContextToResume, &YieldState, &YieldingLuaThread, ResumeFunction(L), NULL, NULL);

				lua_unref(L, YieldedThreadRef);
			});
		}
	}).detach();

	return lua_yield(L, NULL);
}
