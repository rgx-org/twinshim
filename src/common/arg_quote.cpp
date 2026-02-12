#include "common/arg_quote.h"

namespace hklmwrap {

// Matches CreateProcess command-line parsing rules.
std::wstring QuoteWindowsCommandLineArg(const std::wstring& arg) {
  if (arg.empty()) {
    return L"\"\"";
  }

  bool needsQuotes = false;
  for (wchar_t ch : arg) {
    if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' || ch == L'\"') {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) {
    return arg;
  }

  std::wstring out;
  out.push_back(L'\"');
  size_t backslashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') {
      backslashes++;
      continue;
    }
    if (ch == L'\"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    if (backslashes) {
      out.append(backslashes, L'\\');
      backslashes = 0;
    }
    out.push_back(ch);
  }
  if (backslashes) {
    out.append(backslashes * 2, L'\\');
  }
  out.push_back(L'\"');
  return out;
}

std::wstring BuildCommandLine(const std::wstring& exePath, const std::vector<std::wstring>& args) {
  std::wstring cmd = QuoteWindowsCommandLineArg(exePath);
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd.append(QuoteWindowsCommandLineArg(a));
  }
  return cmd;
}

}
