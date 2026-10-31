#pragma once

#if __has_include(<VMProtectSDK.h>)
#  include <VMProtectSDK.h>
#  define VMP_ENABLED 1
#else
#  define VMP_ENABLED 0
inline void VMProtectBegin(const char*) {}
inline void VMProtectBeginVirtualization(const char*) {}
inline void VMProtectBeginMutation(const char*) {}
inline void VMProtectBeginUltra(const char*) {}
inline void VMProtectEnd() {}
inline bool VMProtectIsProtected() { return false; }
inline bool VMProtectIsValidImageCRC() { return true; }
inline const char* VMProtectDecryptStringA(const char* v) { return v; }
#endif
