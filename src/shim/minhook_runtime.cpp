#include "shim/minhook_runtime.h"

#include <MinHook.h>

#include <mutex>

namespace hklmwrap {
namespace {

std::mutex g_mhMutex;
int g_mhRefCount = 0;

}

bool AcquireMinHook() {
  std::lock_guard<std::mutex> lock(g_mhMutex);
  if (g_mhRefCount == 0) {
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
      return false;
    }
  }
  g_mhRefCount++;
  return true;
}

void ReleaseMinHook() {
  std::lock_guard<std::mutex> lock(g_mhMutex);
  if (g_mhRefCount <= 0) {
    return;
  }
  g_mhRefCount--;
  if (g_mhRefCount == 0) {
    (void)MH_Uninitialize();
  }
}

}
