#pragma once

#include <windows.h>

namespace hklmwrap {

bool InstallRegistryHooks();
bool AreRegistryHooksActive();
void RemoveRegistryHooks();

}
