#pragma once

#include "Update/EncryptionsHelper.hpp"

// 745 (0.745.0.7451137). From the 745 struct dump, with the runtime dumper
// confirming LOCVARS (VMValue3), UPVALUES (VMValue4), SOURCE (VMValue1),
// CLOSURE_CONT (VMValue3) and TSTRING_HASH (VMValue2). Proto::p and
// Closure::c.debugname remain plain on this build, so they have no macro.

#define PROTO_LINEINFO_ENC            VMValue3
#define PROTO_ABSLINEINFO_ENC         VMValue4
#define PROTO_LOCVARS_ENC             VMValue3
#define PROTO_UPVALUES_ENC            VMValue4
#define PROTO_SOURCE_ENC              VMValue1
#define PROTO_DEBUGINSN_ENC           VMValue3
#define PROTO_DEBUGNAME_ENC           VMValue3
#define PROTO_TYPEINFO_ENC            VMValue2
#define PROTO_USERDATA_ENC            VMValue1
#define LSTATE_STACKSIZE_ENC          VMValue2
#define CLOSURE_CONT_ENC              VMValue3
#define TSTRING_HASH_ENC              VMValue2
#define UDATA_META_ENC                VMValue2

#define proto_lineinfo         PROTO_LINEINFO_ENC
#define proto_debuginsn        PROTO_DEBUGINSN_ENC
#define proto_typeinfo         PROTO_TYPEINFO_ENC
#define proto_abslineinfo      PROTO_ABSLINEINFO_ENC
#define proto_source           PROTO_SOURCE_ENC
#define proto_locvars          PROTO_LOCVARS_ENC
#define proto_upvalues         PROTO_UPVALUES_ENC
#define proto_debugname        PROTO_DEBUGNAME_ENC
#define proto_userdata         PROTO_USERDATA_ENC
#define udata_meta             UDATA_META_ENC
#define closure_cont           CLOSURE_CONT_ENC
#define tstring_hash           TSTRING_HASH_ENC
#define lstate_stacksize       LSTATE_STACKSIZE_ENC
