#pragma once
#include "steamvr/compatibility.h"
#include <sys/types.h>

namespace ct::helper {
pid_t find_process(const steamvr::compatibility::Binary &binary, bool require_vr = false,
                   uid_t owner = 1000);
}
