#pragma once
#include "windows.h"

#include "lua.h"
#include "lstate.h"

#include "Update/Offsets.hpp"

namespace rbx {
	class c_manager {
	public:
		static void set_proto_capabilities(Proto* proto, uintptr_t* c);
	};

	void rbx::c_manager::set_proto_capabilities(Proto* proto, uintptr_t* c) {
		if (!proto) return;
		proto->userdata = c;
		for (int i = 0; i < proto->sizep; ++i)
			set_proto_capabilities(proto->p[i], c);
	}
}
