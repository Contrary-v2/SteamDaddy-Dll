#pragma once

#include "JsonHelper.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

class GameListManager {
public:
    explicit GameListManager(std::vector<OwnedGameInfo> games, size_t pageSize = 20);

    [[nodiscard]] size_t TotalGames() const noexcept { return m_games.size(); }
    [[nodiscard]] size_t TotalPages() const noexcept;
    [[nodiscard]] size_t CurrentPage() const noexcept { return m_currentPage; }
    [[nodiscard]] size_t PageSize() const noexcept { return m_pageSize; }
    [[nodiscard]] const std::vector<OwnedGameInfo>& Games() const noexcept { return m_games; }
    [[nodiscard]] std::span<const OwnedGameInfo> GetPageItems(size_t pageIndex) const noexcept;

    // Navigates to next page (returns false if already on last page)
    bool NextPage();

    // Navigates to previous page (returns false if already on first page)
    bool PrevPage();

    // Exports full game list to gameslist-<accountName>.csv with UTF-8 BOM
    [[nodiscard]] bool ExportCsv(std::string_view accountName, std::string* outPath = nullptr) const;

private:
    std::vector<OwnedGameInfo> m_games;
    size_t m_pageSize{20};
    size_t m_currentPage{0}; // 0-indexed
};

} // namespace OST::ExtractTickets
