#pragma once

namespace ysm::physics {

// Call before creating or querying physics objects. Concurrent callers wait for initialization.
void InitializeRuntime();

}  // namespace ysm::physics
