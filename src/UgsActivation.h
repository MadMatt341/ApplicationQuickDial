#pragma once

#include "Catalog.h"

namespace quickdial {

// Use UGS's existing Open action when a hidden startup instance cannot accept
// its ordinary singleton activation. False keeps the normal Shell launch path.
bool TryActivateRunningUgs(const ApplicationEntry& application) noexcept;

}  // namespace quickdial
