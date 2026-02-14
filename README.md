# hklm-wrapper

Win32 wrapper that launches a target EXE, injects `hklm_shim.dll`, and virtualizes `HKLM` registry access into a local SQLite DB.

## Binaries

- `hklm_wrapper.exe`: GUI launcher/injector (no console window when launched from GUI tools).
- `hklm_wrapper_cli.exe`: console launcher/injector (recommended for `--debug` from cmd/PowerShell).
- `hklm_shim.dll`: hooked registry layer.
- `hklmreg.exe`: CLI for local DB add/delete/export/import.

Default DB name: `<TargetExeName>-HKLM.sqlite` (next to `hklm_wrapper.exe`).

## Workspace switching (recommended)

Use the workspace file that matches your task:

- `hklm-wrapper-native.code-workspace`
  - macOS/Linux native development and fast unit-test iteration.
- `hklm-wrapper-windows-mingw.code-workspace`
  - Win32-target IntelliSense/cross-build on macOS/Linux.

In VS Code: **File → Open Workspace from File...**

Practical flow:
1. Work mostly in the native workspace.
2. Switch to the MinGW workspace when editing Win32 shim/hooking code.
3. Validate runtime behavior natively on Windows before release.

## Build

This repo uses CMake presets and vcpkg (`sqlite3` comes from `vcpkg.json`).

### macOS/Linux: cross-compile Windows x86 (MinGW)

Install host tools:

- macOS: `brew install cmake ninja git pkg-config mingw-w64`
- Ubuntu/Debian: `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config mingw-w64`

Set up vcpkg:

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

Configure/build:

```bash
cmake --preset windows-x86-mingw-release
cmake --build --preset windows-x86-mingw-release
```

Staging output:

```bash
cmake --preset windows-x86-mingw-release-stage
cmake --build --preset windows-x86-mingw-release-stage
cmake --build --preset windows-x86-mingw-release-stage-install
```

Expected runtime artifacts in `stage/bin`:
- `hklm_wrapper.exe`
- `hklm_wrapper_cli.exe`
- `hklm_shim.dll`
- `hklmreg.exe`

### Windows native (MSVC x86)

Helper scripts:

```cmd
scripts\cmake-msvc-x86.cmd --preset windows-x86-msvc-release
scripts\cmake-msvc-x86.cmd --build --preset windows-x86-msvc-release
```

Staging helper:

```cmd
scripts\build-windows-msvc-x86.cmd
```

## Tests

### Native host tests (macOS/Linux)

```bash
cmake --preset native-tests
cmake --build --preset native-tests
ctest --preset native-tests
```

### Native Windows tests

```cmd
scripts\cmake-msvc-x86.cmd --preset native-tests-windows
scripts\cmake-msvc-x86.cmd --build --preset native-tests-windows
ctest --preset native-tests-windows
```

This suite includes a Windows-only workflow test that launches `hklm_wrapper_cli.exe --debug all` around a probe process and verifies both hook debug trace output and persisted SQLite-backed registry data.

## Run

```text
hklm_wrapper.exe [--debug <api-list|all>] <target_exe> [target args...]
hklm_wrapper_cli.exe [--debug <api-list|all>] <target_exe> [target args...]
```

Use `hklm_wrapper.exe` for normal GUI-driven launches.
Use `hklm_wrapper_cli.exe` when launching from cmd/PowerShell, especially with `--debug`, so the shell blocks until the wrapped process finishes.

Examples:

```text
hklm_wrapper.exe C:\Path\To\TargetApp.exe
hklm_wrapper_cli.exe --debug RegOpenKey,RegQueryValue C:\Path\To\TargetApp.exe
hklm_wrapper_cli.exe --debug all C:\Path\To\TargetApp.exe
```

## hklmreg quick examples

```text
hklmreg --db .\TargetApp-HKLM.sqlite add HKLM\Software\MyApp /v Test /t REG_SZ /d hello /f
hklmreg --db .\TargetApp-HKLM.sqlite delete HKLM\Software\MyApp /v Test /f
hklmreg --db .\TargetApp-HKLM.sqlite export out.reg HKLM\Software\MyApp
hklmreg --db .\TargetApp-HKLM.sqlite import out.reg
```

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

- Wrapper and shim DLL bitness must match target process bitness.
- For native MSVC x86 builds, use `scripts\cmake-msvc-x86.cmd` (or `scripts\build-windows-msvc-x86.cmd`) so the Visual Studio toolchain is initialized with `-arch=x86`. Running CMake directly from an x64 dev shell can produce x64 binaries, which will fail to inject into x86 targets.
- Native MSVC builds default to static runtime (`/MT`) via `HKLM_WRAPPER_MSVC_STATIC_RUNTIME=ON` to avoid conflicts with app-local `MSVCP140.dll`/`vcruntime` in injected target processes.
- Hook mode is runtime-selectable with `HKLM_WRAPPER_HOOK_MODE`:
  - default (unset) / `all` / `full` / `extended`: enable full ANSI + wide hook set
  - `core`/`minimal`/`wide`/`unicode`: wide-only core + legacy/key-info/enum hooks
  - `off`/`none`/`disabled`: inject shim but skip hook installation (diagnostics/fallback)
- macOS/Linux cross-build validates compile/link only; runtime injection/hooking must be validated natively on Windows.
- Hooks a small set of APIs (both `*W` and `*A` where applicable):
  - Open/create keys: `RegOpenKey(Ex)`, `RegCreateKey(Ex)`
  - Close key handles: `RegCloseKey`
  - Read/write values: `RegQueryValue(Ex)`, `RegSetValue(Ex)`, `RegSetKeyValue`
  - Delete keys/values: `RegDeleteValue`, `RegDeleteKey` (and `RegDeleteKeyEx` if present)
  - Enumerate/query metadata: `RegEnumValue`, `RegEnumKey(Ex)`, `RegQueryInfoKey`
- No ACL/security descriptor handling.
- In `--debug` mode, tracing stays active for the full target process lifetime (wrapper waits for target exit).
