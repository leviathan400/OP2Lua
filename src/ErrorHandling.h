#pragma once

namespace OP2Lua::ErrorHandling {

// Installs a custom Windows exception handler to log fatal errors and stacktraces.
void Enable(bool enable = true);

}  // namespace OP2Lua::ErrorHandling
