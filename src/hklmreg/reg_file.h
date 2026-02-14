#pragma once

#include "common/local_registry_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hklmwrap::regfile {

std::wstring CanonKey(const std::wstring& in);

uint32_t ParseType(const std::wstring& t);
std::vector<uint8_t> ParseData(uint32_t type, const std::wstring& dataText);

// Builds a .reg file content (without BOM), using CRLF line endings.
std::wstring BuildRegExportContent(const std::vector<LocalRegistryStore::ExportRow>& rows, const std::wstring& prefix);

// Imports .reg file text already decoded as wide string.
// Returns false on DB failures; tolerates unknown/unsupported lines.
bool ImportRegText(LocalRegistryStore& store, const std::wstring& text);

} // namespace hklmwrap::regfile
