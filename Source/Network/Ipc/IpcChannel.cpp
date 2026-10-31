#include <Network/Ipc/IpcChannel.hpp>
#include <Network/Ipc/PipeCrypto.hpp>
#include <Runtime/Scheduler/TaskScheduler.hpp>
#include <Ui/Overlay/Renderer.hpp>
#include <Core/DebugLog.hpp>
#include <Security/VMProtectShim.h>
#include <Config/Secrets.hpp>

static std::vector<std::pair<ULONG, ULONGLONG>> VerifiedClients;

static ULONGLONG ProcessCreationTime(ULONG Pid)
{
    HANDLE Proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
    if (!Proc) return 0;
    FILETIME Created{}, Exited{}, Kernel{}, User{};
    ULONGLONG Result = 0;
    if (GetProcessTimes(Proc, &Created, &Exited, &Kernel, &User))
        Result = (static_cast<ULONGLONG>(Created.dwHighDateTime) << 32) | Created.dwLowDateTime;
    CloseHandle(Proc);
    return Result;
}

static bool ClientAuthorised(ULONG Pid, const uint8_t* Token)
{
    const ULONGLONG Created = ProcessCreationTime(Pid);
    if (Created != 0)
    {
        for (const auto& Entry : VerifiedClients)
            if (Entry.first == Pid && Entry.second == Created) return true;
    }

    if (!PipeCrypto::ClientHasToken(Pid, Token)) return false;

    if (Created != 0)
    {
        if (VerifiedClients.size() >= 16) VerifiedClients.erase(VerifiedClients.begin());
        VerifiedClients.emplace_back(Pid, Created);
    }
    return true;
}

void PipeServer()
{
    DWORD Pid = GetCurrentProcessId();

    char PipeName[128];
    VMProtectBeginUltra("brand.pipe");
    const char* PipeToken = SUPQ_PIPE_TOKEN_HEX;
    sprintf_s(PipeName, "\\\\.\\pipe\\%s_%lu", PipeToken, Pid);
    VMProtectEnd();

    SECURITY_DESCRIPTOR Sd;
    InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES Sa;
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = &Sd;
    Sa.bInheritHandle = FALSE;

    while (true)
    {
        HANDLE Pipe = CreateNamedPipeA(
            PipeName,
            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            &Sa
        );

        if (Pipe == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_ACCESS_DENIED)
                SUPQ_LOG("pipe: name already owned by another server instance");
            Sleep(250);
            continue;
        }

        BOOL Connected = ConnectNamedPipe(Pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (Connected)
        {
            uint8_t Token[PipeCrypto::kTokenLen];
            PipeCrypto::MaterialiseToken(Token);

            ULONG ClientPid = 0;
            const bool GotPid = GetNamedPipeClientProcessId(Pipe, &ClientPid) != 0;
            const bool ClientOk = GotPid && ClientPid != 0
                                          && ClientPid != GetCurrentProcessId()
                                          && ClientAuthorised(ClientPid, Token);

            if (!ClientOk)
            {
                SUPQ_LOG("pipe: rejecting client PID %lu (whitelist miss)",
                    (unsigned long)ClientPid);
                std::memset(Token, 0, sizeof(Token));
                DisconnectNamedPipe(Pipe);
                CloseHandle(Pipe);
                continue;
            }

            uint32_t NetLen = 0;
            DWORD BytesRead = 0;
            if (ReadFile(Pipe, &NetLen, sizeof(NetLen), &BytesRead, NULL) && BytesRead == sizeof(NetLen))
            {
                uint32_t ScriptLen = ntohl(NetLen);
                if (ScriptLen > 0 && ScriptLen <= (64 * 1024 * 1024))
                {
                    uint8_t Nonce[PipeCrypto::kNonceLen] = {};
                    DWORD NonceRead = 0;
                    const bool NonceOk =
                        ReadFile(Pipe, Nonce, PipeCrypto::kNonceLen, &NonceRead, NULL)
                        && NonceRead == PipeCrypto::kNonceLen;

                    if (NonceOk)
                    {
                        std::vector<uint8_t> Buffer(ScriptLen);
                        size_t Total = 0;
                        bool Ok = true;
                        while (Total < ScriptLen)
                        {
                            DWORD ReadChunk = 0;
                            if (ReadFile(Pipe, Buffer.data() + Total, static_cast<DWORD>(ScriptLen - Total), &ReadChunk, NULL) && ReadChunk > 0)
                            {
                                Total += ReadChunk;
                            }
                            else
                            {
                                Ok = false;
                                break;
                            }
                        }

                        if (Ok && PipeCrypto::DecryptCtr(Token, Nonce, Buffer.data(), Buffer.size()))
                        {
                            std::string Script(reinterpret_cast<const char*>(Buffer.data()), Buffer.size());
                            TaskScheduler::RequestExecution(Script);
                        }
                    }
                }
            }

            std::memset(Token, 0, sizeof(Token));

            DisconnectNamedPipe(Pipe);
        }

        CloseHandle(Pipe);
    }
}

void Communication::Initialize()
{
    std::thread(PipeServer).detach();
}
