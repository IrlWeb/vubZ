#pragma once

// ---------------------------------------------------------------------------
// Build-time secrets
//
// The production pipe token and AES key are intentionally NOT included in this
// source tree. The values below are placeholders so the project still builds.
//
// To build a module that can talk to your loader:
//   1. Generate a new 32-hex-character pipe token and a new 64-hex-character
//      AES token.
//   2. Set them here (or override the macros on the compiler command line).
//   3. Update the loader's twin with the same values.
//
// Do not ship a binary built with the placeholder values.
// ---------------------------------------------------------------------------

#ifndef SUPQ_PIPE_TOKEN_HEX
#define SUPQ_PIPE_TOKEN_HEX "00000000000000000000000000000000"
#endif

#ifndef SUPQ_PIPE_AES_TOKEN_HEX
#define SUPQ_PIPE_AES_TOKEN_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif
