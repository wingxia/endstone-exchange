#pragma once

#include <endstone/nbt/tag.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace exchange {

struct CapturedFrameItem {
    std::string type;
    int data{0};
    endstone::CompoundTag nbt;
};

class StructureReader {
public:
    [[nodiscard]] static std::optional<CapturedFrameItem> readItemFrame(const std::filesystem::path &path);
    [[nodiscard]] static std::optional<CapturedFrameItem>
    readItemFrameFromLevelDbLogs(const std::filesystem::path &database_path, std::string_view structure_name);
};

}  // namespace exchange
