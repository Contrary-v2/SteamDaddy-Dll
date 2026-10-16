#pragma once

#include "Steam/Types.h"

#include <cstddef>
#include <optional>

namespace PendingAPICalls {

    void RecordEncryptedTicket(SteamAPICall_t call, AppId_t appID) noexcept;
    [[nodiscard]] std::optional<AppId_t> TakeEncryptedTicket(SteamAPICall_t call) noexcept;

    void Clear() noexcept;
    [[nodiscard]] size_t GetPendingCount() noexcept;

} // namespace PendingAPICalls
