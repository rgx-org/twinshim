#include "shim/surface_scale_config.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "Addon/AddonDefs.hpp"
#include "Addon/IAddonMainCallback.hpp"
#include "Addon/ID3D12RootObserver.hpp"

namespace {

#include "dgvoodoo_addon/addon_util.inl"
#include "dgvoodoo_addon/addon_types.inl"

class D3D12Observer final : public dgVoodoo::ID3D12RootObserver {
public:
#include "dgvoodoo_addon/d3d12_observer_public.inl"

private:
#include "dgvoodoo_addon/d3d12_observer_private_methods.inl"
#include "dgvoodoo_addon/d3d12_observer_private_state.inl"
};

#include "dgvoodoo_addon/addon_namespace_tail.inl"

} // namespace

#include "dgvoodoo_addon/addon_exports.inl"
