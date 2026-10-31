# supQ Client Module

Client-side native module for the **supQ** Roblox executor. This repository is
the module source only: the supQ loader/attacher, the production secrets, and
the script host are **not** included, so this is not a standalone executor.

## Scope

- Module version: `1.0.0`
- Target client: Roblox **0.745.0.7451137** (`version-5a21b87bbc2641cf`)
- Platform: Windows x64
- Output: `supQ/supQ-Module.dll`

The module is loaded into `RobloxPlayerBeta.exe`. It starts the overlay, the
authenticated pipe server, the RakNet watcher, and the task scheduler, then
attaches to the client's DataModel and builds the Luau execution environment.

### Not included

- The supQ loader / attacher (`supQ.exe` and its injection path)
- The host-side script sender
- The production pipe token and `EmuWL` whitelist token
- Any Roblox binary, dump, or game data

The module cannot attach itself, and it cannot run on its own.

## Build

Requirements:

- Visual Studio with the Desktop C++ workload and the **v145** toolset. For
  VS 2022, change `PlatformToolset` in `supQ.vcxproj` to `v143`.
- Windows SDK and the x64 toolchain.
- The vendored dependencies under `Dependencies/` (already included).

From this directory:

```bat
MSBuild supQ.vcxproj -p:Configuration=Release -p:Platform=x64
```

or open `supQv2.sln`, select `Release | x64`, and build. The DLL is written to
`supQ/supQ-Module.dll`.

At load time the module needs the Microsoft Visual C++ 2015-2022 x64 runtime
(`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`).

## Secrets

The production pipe token, pipe AES token, and `EmuWL` whitelist token are
**not** in this tree. They have been replaced with placeholders:

- [`Source/Config/Secrets.hpp`](Source/Config/Secrets.hpp) — pipe name token
  and pipe AES token.
- [`Source/Security/Pairing.hpp`](Source/Security/Pairing.hpp) — `EmuWL`
  ciphertext/key/tag.

The project builds with the placeholders, but such a build is **not
production-ready**:

- the pipe name and AES key are public placeholder values;
- `EmuWL` fails its tag check and returns `0`, so a loader that enforces the
  pairing gate will reject the module.

To use it with a loader, generate your own values, set them here, and update
the loader's twin (`emulator/include/emulator/pairing.h` for `EmuWL`). Do not
publish a binary built with the placeholders.

## Runtime contract

- Must be loaded into the real 0.745 x64 `RobloxPlayerBeta.exe`; it does not
  work with a different client version or a separate Roblox-compatible client.
- The clone guard expects a process named `supQ.exe`. If that process is not
  present within 20 seconds of the watcher starting, the module terminates the
  client.
- Lua input arrives only through the module's authenticated named pipe. There
  is no built-in console or editor.
- The module's paths (`workspace/`, `autoexecute/`, `supQDecompiler.exe`) are
  resolved from the Roblox executable's directory.

## Version support

All offsets are for Roblox **0.745.0.7451137** only. A new Roblox release
requires new offsets and a rebuild; do not assume the module is version
independent. A failed `SetupExploit` usually means the client build does not
match.

## Project layout

```text
Source/
  Core/        DllMain, logging, shared context
  Runtime/     scheduler, teleport handler, execution guards
  Scripting/   Luau environment and libraries
  Network/     RakNet hooks, IPC pipe, HTTP/WebSocket
  Security/    brand/clone guard, pairing, purchase guard
  Ui/          overlay and renderer
  Update/      offsets, encryptions, FFlag addresses
Dependencies/  vendored third-party libraries
```

## License

Original supQ code is licensed under the MIT License; see [`LICENSE`](LICENSE).
Third-party components remain under their own licenses; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The supQ name and branding
are not licensed for endorsement or for claiming affiliation with Roblox
Corporation.

## Disclaimer

This project is not affiliated with, endorsed by, or sponsored by Roblox
Corporation. It interacts with the Roblox client in ways that may violate
Roblox's Terms of Use. Use at your own risk; there is no warranty.
