#pragma once

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace reportpanel {

inline nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return nlohmann::json::object();
    nlohmann::json value;
    input >> value;
    return value;
}

inline void WriteJsonFileAtomic(const std::filesystem::path& path, const nlohmann::json& value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) throw std::filesystem::filesystem_error("create_directories", path.parent_path(), ec);

    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot open temporary JSON file.");
        const auto bytes = value.dump(2);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("Cannot write JSON file.");
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "MoveFileExW failed");
    }
}

} // namespace reportpanel
