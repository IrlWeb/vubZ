#pragma once
#include <Runtime/Execution/ExecutionGuard.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <Core/DebugLog.hpp>
#include <Core/Utilities.hpp>
#include <Core/Context.hpp>
#include <Scripting/Obfuscation.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Scripting/Environment.hpp>

namespace TpHandler
{
    namespace Detail
    {
        constexpr auto IdlePoll = std::chrono::milliseconds(1000);
        constexpr auto AttachPoll = std::chrono::milliseconds(250);

        constexpr auto MinRetryDelay = std::chrono::milliseconds(250);
        constexpr auto MaxRetryDelay = std::chrono::milliseconds(4000);

        constexpr int MaxSetupFaults = 3;

        constexpr auto FaultCooldown = std::chrono::seconds(20);

        constexpr int FailuresBeforeToast = 5;

        inline std::atomic<bool> Started{ false };
        inline std::atomic<bool> StopRequested{ false };

        inline uintptr_t PlaceIdOffset = Offsets::DataModel::PlaceId;
        constexpr uintptr_t PlaceIdScanLimit = 0x1000;

        inline int64_t ReadPlaceIdSafe(uintptr_t DataModel)
        {
            if (!PlaceIdOffset)
                return -1;

            __try
            {
                return *reinterpret_cast<const int64_t*>(DataModel + PlaceIdOffset);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        inline uintptr_t FindPlaceIdOffset(uintptr_t DataModel, int64_t PlaceId)
        {
            MEMORY_BASIC_INFORMATION Info{};
            if (!VirtualQuery(reinterpret_cast<const void*>(DataModel), &Info, sizeof(Info)) ||
                Info.State != MEM_COMMIT || (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                return 0;

            const uintptr_t RegionEnd = reinterpret_cast<uintptr_t>(Info.BaseAddress) + Info.RegionSize;
            const uintptr_t Limit = RegionEnd - DataModel < PlaceIdScanLimit ? RegionEnd - DataModel : PlaceIdScanLimit;

            __try
            {
                for (uintptr_t Offset = sizeof(uintptr_t); Offset + sizeof(int64_t) <= Limit; Offset += sizeof(int64_t))
                {
                    if (*reinterpret_cast<const int64_t*>(DataModel + Offset) == PlaceId)
                        return Offset;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            return 0;
        }

        inline void CalibratePlaceIdOffset(uintptr_t DataModel)
        {
            const int64_t FromLua = SharedVariables::AttachedLuaPlaceId.load(std::memory_order_acquire);
            if (FromLua <= 0)
                return;

            if (PlaceIdOffset && ReadPlaceIdSafe(DataModel) == FromLua)
                return;

            PlaceIdOffset = FindPlaceIdOffset(DataModel, FromLua);
        }

        inline uintptr_t GetDataModelSafe()
        {
            __try
            {
                return TaskScheduler::GetDataModel();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        inline bool IsInGameSafe(uintptr_t DataModel)
        {
            __try
            {
                return Utils::IsInGame(DataModel);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        inline bool TrySetupExploit(uintptr_t DataModel, bool& Faulted)
        {
            Faulted = false;
            __try
            {
                return TaskScheduler::SetupExploit(DataModel);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Faulted = true;
                return false;
            }
        }

        inline void DrainTeleportQueue()
        {
            std::vector<std::string> Scripts;
            {
                std::lock_guard<std::mutex> Lock(SharedVariables::TeleportQueueMutex);
                Scripts.swap(SharedVariables::TeleportQueue);
            }

            for (std::string& Script : Scripts)
                TaskScheduler::RequestExecution(ExecutionGuard::Wrap(Script));
        }

        inline void InterruptibleSleep(std::chrono::milliseconds Duration)
        {
            constexpr auto Slice = std::chrono::milliseconds(50);
            while (Duration.count() > 0 && !StopRequested.load(std::memory_order_relaxed))
            {
                const auto Step = Duration < Slice ? Duration : Slice;
                std::this_thread::sleep_for(Step);
                Duration -= Step;
            }
        }

        inline uintptr_t GetScriptContextSafe(uintptr_t DataModel)
        {
            __try
            {
                return TaskScheduler::GetScriptContext(DataModel);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        inline void Teardown(const char* Reason)
        {
            if (SharedVariables::LastDataModel == 0 && SharedVariables::ExploitThread == nullptr)
                return;

            SUPQ_LOG("TpHandler: teardown (%s)", Reason);

            SharedVariables::LastDataModel = 0;
            SharedVariables::ExploitThread = nullptr;
            SharedVariables::ExecutorHeartbeatConnection.store(0, std::memory_order_release);
            SharedVariables::StateGeneration.fetch_add(1, std::memory_order_release);

            {
                std::lock_guard<std::mutex> Lock(SharedVariables::ExecutionRequestsMutex);
                SharedVariables::ExecutionRequests.clear();
            }

            Environment::TeardownEnvironment();
        }

        inline void Run()
        {
            SUPQ_LOG_OK("TpHandler::Run started - polling for DataModel");

            int LastStage = -1;
            int Ticks = 0;
            auto Stage = [&](int Id, const char* Text)
            {
                ++Ticks;
                if (Id != LastStage)
                {
                    SUPQ_LOG("%s", Text);
                    LastStage = Id;
                    Ticks = 0;
                }
                else if (Ticks % 15 == 0)
                {
                    SUPQ_LOG("  ... still: %s (%d polls)", Text, Ticks);
                }
            };

            auto RetryDelay = MinRetryDelay;

            uintptr_t LastScriptContext = 0;

            uintptr_t FaultingDataModel = 0;
            int SetupFaults = 0;
            auto FaultCooldownUntil = std::chrono::steady_clock::now();

            int ConsecutiveFailures = 0;
            bool FailureNotified = false;

            uintptr_t LastLiveDataModel = 0;
            auto ResetFailureStreak = [&]
            {
                ConsecutiveFailures = 0;
                FailureNotified = false;
            };

            while (!StopRequested.load(std::memory_order_relaxed))
            {
                const uintptr_t DataModel = GetDataModelSafe();

                if (!DataModel)
                {
                    Teardown("DataModel gone");
                    LastScriptContext = 0;
                    ResetFailureStreak();
                    Stage(1, "waiting: no DataModel yet");
                    InterruptibleSleep(AttachPoll);
                    continue;
                }

                if (!IsInGameSafe(DataModel))
                {
                    Teardown("DataModel left game");
                    LastScriptContext = 0;
                    ResetFailureStreak();
                    Stage(2, "waiting: IsInGame false");
                    InterruptibleSleep(AttachPoll);
                    continue;
                }

                if (PlaceIdOffset && ReadPlaceIdSafe(DataModel) == 0)
                {
                    Teardown("in lobby");
                    LastScriptContext = 0;
                    ResetFailureStreak();
                    Stage(5, "waiting: lobby (PlaceId 0)");
                    InterruptibleSleep(AttachPoll);
                    continue;
                }

                const uintptr_t ScriptContext = GetScriptContextSafe(DataModel);

                if (DataModel == SharedVariables::LastDataModel &&
                    ScriptContext != 0 && ScriptContext == LastScriptContext)
                {
                    RetryDelay = MinRetryDelay;
                    Stage(3, "live");
                    InterruptibleSleep(IdlePoll);
                    continue;
                }

                if (DataModel != FaultingDataModel)
                {
                    FaultingDataModel = 0;
                    SetupFaults = 0;
                }

                if (SetupFaults >= MaxSetupFaults)
                {
                    if (std::chrono::steady_clock::now() < FaultCooldownUntil)
                    {
                        Stage(4, "cooling down after repeated SetupExploit faults");
                        InterruptibleSleep(IdlePoll);
                        continue;
                    }

                    SUPQ_LOG("TpHandler: cooldown elapsed, retrying SetupExploit");
                    SetupFaults = 0;
                    FaultingDataModel = 0;
                }

                SUPQ_LOG("DataModel 0x%llX is in game - calling SetupExploit",
                    (unsigned long long)DataModel);
                LastStage = -1;

                bool Faulted = false;
                if (!TrySetupExploit(DataModel, Faulted))
                {
                    SUPQ_LOG_ERR("SetupExploit FAILED (faulted=%d, fault count %d)",
                        (int)Faulted, SetupFaults + (Faulted ? 1 : 0));

                    ++ConsecutiveFailures;
                    bool EnteringCooldown = false;

                    if (Faulted)
                    {
                        FaultingDataModel = DataModel;
                        ++SetupFaults;
                        if (SetupFaults >= MaxSetupFaults)
                        {
                            FaultCooldownUntil = std::chrono::steady_clock::now() + FaultCooldown;
                            EnteringCooldown = true;
                        }
                    }

                    if (DataModel != LastLiveDataModel && !FailureNotified &&
                        (EnteringCooldown || ConsecutiveFailures >= FailuresBeforeToast))
                    {
                        FailureNotified = true;
                        TaskScheduler::NotifySetupFailed(EnteringCooldown
                            ? SUPQ2_OBF("Setup crashed. Retrying in 20s...")
                            : SUPQ2_OBF("supQ couldn't load. (Ignore if youre in the lobby)"));
                    }

                    InterruptibleSleep(RetryDelay);
                    RetryDelay = RetryDelay * 2 > MaxRetryDelay ? MaxRetryDelay : RetryDelay * 2;
                    continue;
                }

                SUPQ_LOG_OK("SetupExploit SUCCEEDED - executor is live");

                SharedVariables::LastDataModel = DataModel;
                LastScriptContext = GetScriptContextSafe(DataModel);

                LastLiveDataModel = DataModel;
                CalibratePlaceIdOffset(DataModel);

                RetryDelay = MinRetryDelay;
                FaultingDataModel = 0;
                SetupFaults = 0;
                ResetFailureStreak();

                DrainTeleportQueue();

                InterruptibleSleep(IdlePoll);
            }

            Started.store(false, std::memory_order_release);
        }
    }

    inline void Initialize()
    {
        bool Expected = false;
        if (!Detail::Started.compare_exchange_strong(Expected, true))
            return;

        Detail::StopRequested.store(false, std::memory_order_relaxed);
        std::thread(&Detail::Run).detach();
    }

    inline void Shutdown()
    {
        Detail::StopRequested.store(true, std::memory_order_relaxed);
    }
}
