#pragma once
#include <string>

namespace gage::reg {

    // All GAGE-MP settings live under HKCU\Software\GAGE-MP (string values).

    // Read a string value; `def` when the key/value is absent, not a string
    // type, or unreadable. REG_EXPAND_SZ values are returned expanded.
    std::wstring Get(const std::wstring& key, const std::wstring& def = L"");

    // Write a REG_SZ value. False (and a logged error) on failure.
    bool         Set(const std::wstring& key, const std::wstring& value);

    bool         Has(const std::wstring& key);

    // Delete a value. An absent value counts as success.
    bool         Clear(const std::wstring& key);

}
