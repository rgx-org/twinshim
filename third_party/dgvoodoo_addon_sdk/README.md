# dgVoodoo AddOn SDK

This folder is expected to contain the unpacked dgVoodoo SDK.

The AddOn target uses the SDK's default layout:

- Headers: `Inc/Addon/*.hpp`
- Libraries: `Lib/<arch>/dgVoodooAddon.lib`

Build notes:

- The CMake option `HKLM_WRAPPER_ENABLE_DGVOODOO_ADDON` currently requires **MSVC** because the SDK ships MSVC `.lib` files.
- dgVoodoo searches for an add-on DLL named `SampleAddon.dll`; the CMake target outputs that name.

Override paths if needed:

- `-DDGVOODOO_ADDON_SDK_DIR=<path>`
- `-DDGVOODOO_ADDON_INC_DIR=<path-to-Inc>`
- `-DDGVOODOO_ADDON_LIB_DIR=<path-to-Lib/x86>`
