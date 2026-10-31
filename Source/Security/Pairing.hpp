#pragma once

// ============================================================================
//  EmuWL whitelist (module side)
//
//  The only thing this side owes the host: the EmuWL export, which hands the
//  runtime-decoded whitelist token to the emulator's injection gate. The gate
//  kills every foreign module and every supQ-Module.dll that cannot answer
//  EmuWL. There is intentionally NO host check on this side - the emulator
//  checks the module, never the other way around.
//
//  THIS HEADER IS A TWIN. The TWIN CORE B block below MUST stay byte-identical
//  to emulator/include/emulator/pairing.h in the emulator tree. Change one,
//  change both, or every client fails the gate. In this public source tree the
//  token is a placeholder; set both copies to the same new value before
//  building a deployable module.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace Pairing
{
    // --- TWIN CORE B / EmuWL whitelist (keep identical to the emulator header)
    //
    // The production EmuWL token is intentionally NOT included in this source
    // tree. The values below are placeholders, so `wlDecode` fails its FNV tag
    // check and the EmuWL export returns 0. A loader that enforces the gate
    // will reject this build until you supply your own token here and in the
    // loader's `emulator/include/emulator/pairing.h` twin.
    //
    // Generate your own 14-byte ciphertext/key/tag; do not reuse the production
    // values from a private build.
    inline constexpr std::size_t kWlLen = 14;
    inline constexpr unsigned char kWlEnc[kWlLen] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    inline constexpr unsigned char kWlKey[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    inline constexpr std::uint64_t kWlTag = 0ull;

    // Runtime-only decode. Returns false and zeroes Out when the tag does not
    // match (someone edited the ciphertext, the key, or this routine).
    inline bool
    wlDecode(unsigned char Out[kWlLen])
    {
        unsigned char tmp[kWlLen];
        std::uint64_t h = 0xCBF29CE484222325ull;

        for (std::size_t i = 0; i < kWlLen; ++i) {
            tmp[i] = static_cast<unsigned char>(
                kWlEnc[i] ^ kWlKey[(i * 7u + 5u) & 15u] ^
                static_cast<unsigned char>((0x5Cu + i * 0x29u) & 0xFFu));
        }
        for (std::size_t i = 0; i < kWlLen; ++i) {
            h ^= static_cast<std::uint64_t>(tmp[i]);
            h *= 0x100000001B3ull;
        }
        const bool ok = (h == kWlTag);
        for (std::size_t i = 0; i < kWlLen; ++i) {
            Out[i] = ok ? tmp[i] : 0u;
            tmp[i] = 0u;
        }
        return ok;
    }
    // --- END TWIN CORE B ---------------------------------------------------

    // Body of the EmuWL export (defined in entry.cpp): hands our decoded
    // token to the host. Never logs it, never returns it on failure.
    inline int EmuWL_Write(unsigned char *Out, unsigned Cap)
    {
        if (!Out || Cap < kWlLen) {
            return 0;
        }
        if (!wlDecode(Out)) {
            SecureZeroMemory(Out, kWlLen);
            return 0;
        }
        return static_cast<int>(kWlLen);
    }
}
