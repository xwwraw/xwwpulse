//  A small TOML reader for the subset xwwPulse writes: [tables],
//  key = value with strings ("basic" and 'literal'), integers, floats,
//  booleans and arrays of those (multi-line arrays and comments included).
//  Keys come back fully qualified: "gpu.priority".
#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace xww::toml {

using Array = std::vector<std::wstring>;               // arrays keep their items as text
using Value = std::variant<bool, long long, double, std::wstring, Array>;

struct Document {
    std::map<std::wstring, Value> values;
    std::vector<std::wstring> errors;                  // "line 12: ..."

    bool Bool(const std::wstring& key, bool fallback) const;
    long long Int(const std::wstring& key, long long fallback) const;
    double Double(const std::wstring& key, double fallback) const;
    std::wstring String(const std::wstring& key, const std::wstring& fallback) const;
    Array List(const std::wstring& key, const Array& fallback) const;
};

Document Parse(const std::wstring& text);
Document ParseFile(const std::wstring& path);
std::wstring Quote(const std::wstring& s);             // "basic string" with escapes

}  // namespace xww::toml
