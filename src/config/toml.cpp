#include "config/toml.h"
#include "core/common.h"

#include <cwctype>

namespace xww::toml {

namespace {

struct Reader {
    const std::wstring& text;
    size_t pos = 0;
    int line = 1;

    bool End() const { return pos >= text.size(); }
    wchar_t Peek() const { return End() ? L'\0' : text[pos]; }
    wchar_t Next() { wchar_t c = Peek(); if (!End()) { ++pos; if (c == L'\n') ++line; } return c; }

    void SkipSpace() { while (!End() && (Peek() == L' ' || Peek() == L'\t')) ++pos; }
    void SkipComment() { if (Peek() == L'#') while (!End() && Peek() != L'\n') ++pos; }
    // spaces, comments and newlines (inside arrays)
    void SkipAll() { for (;;) { SkipSpace(); SkipComment(); if (Peek() == L'\n' || Peek() == L'\r') Next(); else break; } }
    void SkipLine() { while (!End() && Peek() != L'\n') ++pos; Next(); }

    std::wstring Key() {
        SkipSpace();
        if (Peek() == L'"' || Peek() == L'\'') return QuotedString();
        std::wstring key;
        while (!End() && (std::iswalnum(Peek()) || Peek() == L'_' || Peek() == L'-' || Peek() == L'.')) key += Next();
        return key;
    }

    std::wstring QuotedString() {
        wchar_t quote = Next();
        std::wstring s;
        while (!End() && Peek() != quote && Peek() != L'\n') {
            wchar_t c = Next();
            if (quote == L'"' && c == L'\\') {
                wchar_t e = Next();
                switch (e) {
                case L'n': s += L'\n'; break;
                case L't': s += L'\t'; break;
                case L'r': s += L'\r'; break;
                case L'"': s += L'"'; break;
                case L'\\': s += L'\\'; break;
                case L'u': { std::wstring hex = text.substr(pos, 4); pos += 4; s += static_cast<wchar_t>(std::wcstoul(hex.c_str(), nullptr, 16)); break; }
                default: s += e;
                }
            } else s += c;
        }
        if (Peek() == quote) Next();
        return s;
    }

    std::optional<Value> Scalar() {
        if (Peek() == L'"' || Peek() == L'\'') return Value(QuotedString());
        std::wstring token;
        while (!End() && Peek() != L',' && Peek() != L']' && Peek() != L'#' && Peek() != L'\n' && Peek() != L'\r') token += Next();
        token = Trim(token);
        if (token == L"true") return Value(true);
        if (token == L"false") return Value(false);
        if (token.empty()) return std::nullopt;
        std::wstring digits;
        for (wchar_t c : token) if (c != L'_') digits += c;
        wchar_t* end = nullptr;
        if (digits.find_first_of(L".eE") == std::wstring::npos) {
            long long n = std::wcstoll(digits.c_str(), &end, 0);
            if (end && *end == L'\0') return Value(n);
        }
        double d = std::wcstod(digits.c_str(), &end);
        if (end && *end == L'\0') return Value(d);
        return std::nullopt;
    }

    std::optional<Value> Parse() {
        SkipSpace();
        if (Peek() != L'[') return Scalar();
        Next();
        Array items;
        for (;;) {
            SkipAll();
            if (Peek() == L']') { Next(); break; }
            if (End()) return std::nullopt;
            auto item = Scalar();
            if (!item) return std::nullopt;
            if (auto s = std::get_if<std::wstring>(&*item)) items.push_back(*s);
            else if (auto b = std::get_if<bool>(&*item)) items.push_back(*b ? L"true" : L"false");
            else if (auto i = std::get_if<long long>(&*item)) items.push_back(std::to_wstring(*i));
            else if (auto f = std::get_if<double>(&*item)) items.push_back(std::to_wstring(*f));
            SkipAll();
            if (Peek() == L',') Next();
        }
        return Value(std::move(items));
    }
};

}  // namespace

Document Parse(const std::wstring& text) {
    Document doc;
    Reader r{ text };
    std::wstring table;
    while (!r.End()) {
        r.SkipSpace();
        r.SkipComment();
        wchar_t c = r.Peek();
        if (c == L'\n' || c == L'\r') { r.Next(); continue; }
        if (r.End()) break;
        if (c == L'[') {
            r.Next();
            table = r.Key();
            r.SkipSpace();
            if (r.Peek() != L']') doc.errors.push_back(std::format(L"line {}: expected ]", r.line));
            r.SkipLine();
            continue;
        }
        int line = r.line;
        auto key = r.Key();
        r.SkipSpace();
        if (key.empty() || r.Peek() != L'=') { doc.errors.push_back(std::format(L"line {}: expected key = value", line)); r.SkipLine(); continue; }
        r.Next();
        auto value = r.Parse();
        if (!value) { doc.errors.push_back(std::format(L"line {}: unreadable value for {}", line, key)); r.SkipLine(); continue; }
        doc.values[table.empty() ? key : table + L"." + key] = std::move(*value);
        r.SkipSpace();
        r.SkipComment();
        if (!r.End() && r.Peek() != L'\n' && r.Peek() != L'\r') doc.errors.push_back(std::format(L"line {}: text after the value", line));
        r.SkipLine();
    }
    return doc;
}

Document ParseFile(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file.get(), &size);
    std::string data(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 1 << 20)), '\0');
    DWORD read = 0;
    ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    data.resize(read);
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    return Parse(Widen(data));
}

std::wstring Quote(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\t') out += L"\\t";
        else out += c;
    }
    return out + L"\"";
}

template <class T> static const T* Find(const Document& d, const std::wstring& key) {
    auto it = d.values.find(key);
    return it == d.values.end() ? nullptr : std::get_if<T>(&it->second);
}

bool Document::Bool(const std::wstring& key, bool fallback) const { auto v = Find<bool>(*this, key); return v ? *v : fallback; }
long long Document::Int(const std::wstring& key, long long fallback) const { auto v = Find<long long>(*this, key); return v ? *v : fallback; }
double Document::Double(const std::wstring& key, double fallback) const {
    if (auto v = Find<double>(*this, key)) return *v;
    if (auto i = Find<long long>(*this, key)) return static_cast<double>(*i);
    return fallback;
}
std::wstring Document::String(const std::wstring& key, const std::wstring& fallback) const { auto v = Find<std::wstring>(*this, key); return v ? *v : fallback; }
Array Document::List(const std::wstring& key, const Array& fallback) const { auto v = Find<Array>(*this, key); return v ? *v : fallback; }

}  // namespace xww::toml
