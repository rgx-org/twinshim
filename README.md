# hklm-wrapper

Simple Win32 wrapper that launches a target EXE supplied on the command line and virtualizes **HKLM** registry access into a per-target SQLite database.

## What you get

- `hklm_wrapper.exe` (GUI launcher): starts the provided target process, injects `hklm_shim.dll`, and forwards all trailing arguments.
- `hklm_shim.dll` (injected): hooks common `Reg*W` APIs and stores HKLM writes/deletes into a SQLite DB; reads prefer the local DB and fall back to the real registry.
- `hklmreg.exe` (CLI): basic `REG`-like utility to `add`, `delete`, `export`, `import` values in the local HKLM store.

The SQLite database name is: `<TargetExeName>-HKLM.sqlite` and is created next to `hklm_wrapper.exe`.

## Configure (compile-time)

Edit [config/wrapper_config.h](config/wrapper_config.h) as needed:

- `HKLM_WRAPPER_WORKING_DIR` (optional override; default is target EXE directory)
- `HKLM_WRAPPER_SHIM_DLL_NAME`
- `HKLM_WRAPPER_IGNORE_EMBEDDED_MANIFEST` (default `1`; sets `__COMPAT_LAYER=RunAsInvoker` when launching target)

## Build

This project uses CMake + vcpkg for SQLite3.

### Native Windows build

For a 32-bit Windows build (Win32):

1) Install vcpkg and set `VCPKG_ROOT`.

2) Configure + build:

`cmake -S . -B build-win32 -A Win32 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x86-windows`

`cmake --build build-win32 --config Release`

Outputs include `hklm_wrapper.exe`, `hklm_shim.dll`, and `hklmreg.exe`.

### Cross-compile from macOS/Linux to Win32 (x86)

The repo includes:

- `cmake/toolchains/mingw-w64-i686.cmake` (Win32 target toolchain)
- `CMakePresets.json` preset `windows-x86-mingw-release`

#### 1) Install required host tooling

macOS (Homebrew):

`brew install cmake ninja git pkg-config mingw-w64`

Linux (Debian/Ubuntu):

`sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config mingw-w64`

#### 2) Install vcpkg and export `VCPKG_ROOT`

`git clone https://github.com/microsoft/vcpkg.git ~/vcpkg`

`~/vcpkg/bootstrap-vcpkg.sh`

`export VCPKG_ROOT=~/vcpkg`

If `VCPKG_ROOT` is not set, the provided CMake presets automatically fall back to `~/vcpkg`.

#### 3) Configure + build with preset

`cmake --preset windows-x86-mingw-release`

`cmake --build --preset windows-x86-mingw-release`

Artifacts are emitted under `build/windows-x86-mingw-release`.

### Staging package output (x86 MinGW release)

Use the staging preset flow to produce only runtime binaries in a stable directory:

`cmake --preset windows-x86-mingw-release-stage`

`cmake --build --preset windows-x86-mingw-release-stage`

`cmake --build --preset windows-x86-mingw-release-stage-install`

Expected output path:

- `stage/bin/hklm_wrapper.exe`
- `stage/bin/hklm_shim.dll`
- `stage/bin/hklmreg.exe`

Optional presets:

- `windows-x86-mingw-debug`
- `windows-x64-mingw-release`

## Unit tests

The repository includes a CTest/Catch2 unit test setup under `tests/`.

Native host test run (macOS/Linux/Windows):

`cmake -S . -B build-tests -G Ninja`

`cmake --build build-tests`

`ctest --test-dir build-tests --output-on-failure`

Preset-based test run:

`cmake --preset native-tests`

`cmake --build --preset native-tests`

`ctest --preset native-tests`

Notes:

- Core tests (`hklm_common_tests`) always build.
- Store tests (`hklm_store_tests`) build when SQLite3 is available to CMake.

## Run

- Put `hklm_wrapper.exe` and `hklm_shim.dll` next to each other.
- Run `hklm_wrapper.exe <target_exe> [target arguments...]`.
- Optional debug tracing: `hklm_wrapper.exe --debug <api1,api2,...|all> <target_exe> [target arguments...]`.
- By default, the wrapper launches targets with `RunAsInvoker` compatibility so embedded elevation manifests are ignored.

Examples:

`hklm_wrapper.exe C:\\Path\\To\\TargetApp.exe`

`hklm_wrapper.exe C:\\Path\\To\\TargetApp.exe --mode test --config "C:\\path with spaces\\cfg.json"`

`hklm_wrapper.exe --debug RegOpenKey,RegQueryValue C:\\Path\\To\\TargetApp.exe`

`hklm_wrapper.exe --debug all C:\\Path\\To\\TargetApp.exe`

`--debug` prints a line to stdout each time a selected hooked API is called in the target process.
For base names, non-`Ex` and `Ex` are both matched (for example `RegQueryValue` matches `RegQueryValue` and `RegQueryValueEx`; `RegOpenKey` matches `RegOpenKey` and `RegOpenKeyEx`).

## hklmreg examples

`hklmreg --db .\\TargetApp-HKLM.sqlite add HKLM\\Software\\MyApp /v Test /t REG_SZ /d hello /f`

`hklmreg --db .\\TargetApp-HKLM.sqlite delete HKLM\\Software\\MyApp /v Test /f`

`hklmreg --db .\\TargetApp-HKLM.sqlite export out.reg HKLM\\Software\\MyApp`

`hklmreg --db .\\TargetApp-HKLM.sqlite import out.reg`

## SQLite schema (direct DB format)

The local store uses two tables:

### `keys`

Tracks key-level existence/tombstones.

```sql
CREATE TABLE keys(
  key_path   TEXT PRIMARY KEY,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);
```

- `key_path`: canonical key path (for example `HKLM\\Software\\MyApp`).
- `is_deleted`: soft-delete flag (`0` active, `1` deleted/tombstone).
- `updated_at`: Unix epoch seconds.

### `values_tbl`

Stores registry values and value-level tombstones.

```sql
CREATE TABLE values_tbl(
  key_path   TEXT NOT NULL,
  value_name TEXT NOT NULL,
  type       INTEGER NOT NULL,
  data       BLOB,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY(key_path, value_name)
);
CREATE INDEX idx_values_key ON values_tbl(key_path);
```

- `value_name`: empty string means the default value (`(Default)`).
- `type`: Win32 registry type value (common: `REG_SZ=1`, `REG_BINARY=3`, `REG_DWORD=4`, `REG_QWORD=11`).
- `data`: raw bytes for that type:
  - `REG_SZ`: UTF-16LE bytes including terminating NUL.
  - `REG_DWORD`: 4-byte little-endian integer.
  - `REG_QWORD`: 8-byte little-endian integer.
- `is_deleted`/`updated_at`: same semantics as `keys`.

Deletes are modeled as tombstones (`is_deleted=1`) rather than hard row removal.

## Direct SQLite examples

Examples below use the `sqlite3` CLI directly.

### Insert a `REG_SZ` value (`Test=hello`)

```sql
INSERT INTO keys(key_path, is_deleted, updated_at)
VALUES ('HKLM\\Software\\MyApp', 0, strftime('%s','now'))
ON CONFLICT(key_path) DO UPDATE SET
  is_deleted=0,
  updated_at=excluded.updated_at;

INSERT INTO values_tbl(key_path, value_name, type, data, is_deleted, updated_at)
VALUES (
  'HKLM\\Software\\MyApp',
  'Test',
  1,
  X'680065006C006C006F000000',
  0,
  strftime('%s','now')
)
ON CONFLICT(key_path, value_name) DO UPDATE SET
  type=excluded.type,
  data=excluded.data,
  is_deleted=0,
  updated_at=excluded.updated_at;
```

### Query active values under a key

```sql
SELECT
  key_path,
  value_name,
  type,
  hex(data) AS data_hex,
  datetime(updated_at, 'unixepoch') AS updated_utc
FROM values_tbl
WHERE key_path = 'HKLM\\Software\\MyApp'
  AND is_deleted = 0
ORDER BY value_name;
```

### Insert/query a `REG_DWORD` value (`Flags=123`)

`123` decimal = `0x7B`, stored little-endian as `X'7B000000'`.

```sql
INSERT INTO values_tbl(key_path, value_name, type, data, is_deleted, updated_at)
VALUES ('HKLM\\Software\\MyApp', 'Flags', 4, X'7B000000', 0, strftime('%s','now'))
ON CONFLICT(key_path, value_name) DO UPDATE SET
  type=excluded.type,
  data=excluded.data,
  is_deleted=0,
  updated_at=excluded.updated_at;

SELECT
  value_name,
  hex(data) AS dword_le_hex
FROM values_tbl
WHERE key_path='HKLM\\Software\\MyApp'
  AND value_name='Flags'
  AND type=4
  AND is_deleted=0;
```

## Notes / limitations

- Hooks a small set of APIs (both `*W` and `*A` where applicable):
  - Open/create keys: `RegOpenKey(Ex)`, `RegCreateKey(Ex)`
  - Close key handles: `RegCloseKey`
  - Read/write values: `RegQueryValue(Ex)`, `RegSetValue(Ex)`, `RegSetKeyValue`
  - Delete keys/values: `RegDeleteValue`, `RegDeleteKey` (and `RegDeleteKeyEx` if present)
  - Enumerate/query metadata: `RegEnumValue`, `RegEnumKey(Ex)`, `RegQueryInfoKey`
- No ACL/security descriptor handling.
- Hooks both wide and ANSI variants for the functions above (`*W` and `*A`).
- In `--debug` mode, tracing stays active for the full target process lifetime (wrapper waits for target exit).
- Wrapper + shim must match the target bitness (x86 target needs x86 wrapper+DLL; x64 target needs x64 wrapper+DLL).
- Cross-compiling on macOS/Linux only builds Windows binaries; run/test them on Windows (or under Wine).