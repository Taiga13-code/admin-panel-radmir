#pragma once

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace reportpanel {

inline std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

inline std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

inline bool IsValidUtf8(std::string_view value) {
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

enum class TextEncoding {
    Detect,
    Utf8,
    Windows1251,
    Utf16LittleEndian,
    Utf16BigEndian
};

inline std::string DecodeLineToUtf8(std::string_view input, TextEncoding encoding) {
    std::wstring wide;
    if (encoding == TextEncoding::Utf16LittleEndian ||
        encoding == TextEncoding::Utf16BigEndian) {
        const bool little = encoding == TextEncoding::Utf16LittleEndian;
        std::size_t i = 0;
        if (input.size() >= 2) {
            const auto a = static_cast<unsigned char>(input[0]);
            const auto b = static_cast<unsigned char>(input[1]);
            if ((little && a == 0xFF && b == 0xFE) ||
                (!little && a == 0xFE && b == 0xFF)) i = 2;
        }
        for (; i + 1 < input.size(); i += 2) {
            const auto a = static_cast<unsigned char>(input[i]);
            const auto b = static_cast<unsigned char>(input[i + 1]);
            const wchar_t ch = static_cast<wchar_t>(little ? (a | (b << 8)) : ((a << 8) | b));
            if (ch != L'\0') wide.push_back(ch);
        }
        if (!wide.empty() && wide.back() == L'\r') wide.pop_back();
        return WideToUtf8(wide);
    }

    if (encoding == TextEncoding::Detect) {
        if (input.size() >= 3 &&
            static_cast<unsigned char>(input[0]) == 0xEF &&
            static_cast<unsigned char>(input[1]) == 0xBB &&
            static_cast<unsigned char>(input[2]) == 0xBF) {
            input.remove_prefix(3);
        }
        if (IsValidUtf8(input)) return std::string(input);
        encoding = TextEncoding::Windows1251;
    }
    if (encoding == TextEncoding::Utf8) {
        if (input.size() >= 3 &&
            static_cast<unsigned char>(input[0]) == 0xEF &&
            static_cast<unsigned char>(input[1]) == 0xBB &&
            static_cast<unsigned char>(input[2]) == 0xBF) {
            input.remove_prefix(3);
        }
        return std::string(input);
    }

    const int size = MultiByteToWideChar(1251, 0, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return {};
    wide.resize(static_cast<std::size_t>(size));
    MultiByteToWideChar(1251, 0, input.data(), static_cast<int>(input.size()),
                        wide.data(), size);
    if (!wide.empty() && wide.back() == L'\r') wide.pop_back();
    return WideToUtf8(wide);
}

} // namespace reportpanel
