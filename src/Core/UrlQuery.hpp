#pragma once

#include <string>
#include <string_view>

[[nodiscard]] inline int HexDigitValue(const char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

[[nodiscard]] inline std::string UrlDecode(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%' && i + 2 < value.size())
        {
            const int high = HexDigitValue(value[i + 1]);
            const int low = HexDigitValue(value[i + 2]);
            if (high >= 0 && low >= 0)
            {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

[[nodiscard]] inline std::string GetUrlQueryParam(const std::string_view url, const std::string_view key)
{
    const std::string needle = std::string(key) + "=";
    size_t begin = url.find('?');
    begin = begin == std::string_view::npos ? 0 : begin + 1;

    while (begin < url.size())
    {
        const size_t end = url.find_first_of("&#", begin);
        const std::string_view item = url.substr(begin, end == std::string_view::npos ? url.size() - begin : end - begin);
        if (item.starts_with(needle))
            return UrlDecode(item.substr(needle.size()));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return {};
}
