#pragma once

#include <string_view>

namespace taskforge {

[[nodiscard]] constexpr std::string_view project_name() noexcept {
    return "taskforge";
}

}  // namespace taskforge
