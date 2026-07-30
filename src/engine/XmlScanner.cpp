#include "XmlScanner.h"

#include <algorithm>

namespace {

bool isNameChar(char c) { return XmlScanner::Detail::isNameChar(c); }

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

bool XmlScanner::firstAttribute(std::string_view raw,
                                std::string_view* name, std::string_view* value)
{
    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i; // element name

    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') return false;

    const size_t nameStart = i;
    while (i < raw.size() && isNameChar(raw[i])) ++i;
    if (i == nameStart) return false;
    *name = raw.substr(nameStart, i - nameStart);

    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size() || raw[i] != '=') { *value = {}; return true; }
    ++i;
    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size()) { *value = {}; return true; }

    if (raw[i] == '"' || raw[i] == '\'') {
        const char quote = raw[i++];
        const size_t start = i;
        while (i < raw.size() && raw[i] != quote) ++i;
        *value = raw.substr(start, i - start);
    } else {
        const size_t start = i;
        while (i < raw.size() && !isSpace(raw[i]) && raw[i] != '>' && raw[i] != '/') ++i;
        *value = raw.substr(start, i - start);
    }
    return true;
}

std::string_view XmlScanner::attributeValue(std::string_view raw, std::string_view attr)
{
    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i;

    while (i < raw.size()) {
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') break;

        const size_t nameStart = i;
        while (i < raw.size() && isNameChar(raw[i])) ++i;
        if (i == nameStart) { ++i; continue; }
        const std::string_view name = raw.substr(nameStart, i - nameStart);

        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] != '=') continue;
        ++i;
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size()) break;

        std::string_view value;
        if (raw[i] == '"' || raw[i] == '\'') {
            const char quote = raw[i++];
            const size_t start = i;
            while (i < raw.size() && raw[i] != quote) ++i;
            value = raw.substr(start, i - start);
            if (i < raw.size()) ++i;
        } else {
            const size_t start = i;
            while (i < raw.size() && !isSpace(raw[i]) && raw[i] != '>' && raw[i] != '/') ++i;
            value = raw.substr(start, i - start);
        }
        if (name == attr) return value;
    }
    return {};
}
