#include "GameListManager.h"
#include "I18n.h"
#include "Log.h"
#include "Utils.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace OST::ExtractTickets {

namespace {
    std::string EscapeCsv(std::string_view str) {
        std::string cleaned;
        cleaned.reserve(str.size());
        for (char c : str) {
            const auto uc = static_cast<unsigned char>(c);
            if (uc < 0x20 && c != '\r' && c != '\n' && c != '\t') {
                continue;
            }
            cleaned.push_back(c);
        }

        const bool needsQuotes = cleaned.find_first_of(",\"\r\n\t") != std::string::npos;
        if (!needsQuotes) {
            return cleaned;
        }

        std::string escaped = "\"";
        escaped.reserve(cleaned.size() + 4);
        for (char c : cleaned) {
            if (c == '"') {
                escaped += "\"\"";
            } else {
                escaped.push_back(c);
            }
        }
        escaped += "\"";
        return escaped;
    }
} // namespace

GameListManager::GameListManager(std::vector<OwnedGameInfo> games, size_t pageSize)
    : m_games(std::move(games)), m_pageSize(pageSize ? pageSize : 20), m_currentPage(0) {
}

size_t GameListManager::TotalPages() const noexcept {
    if (m_games.empty()) return 1;
    return (m_games.size() + m_pageSize - 1) / m_pageSize;
}

bool GameListManager::NextPage() {
    if (m_currentPage + 1 < TotalPages()) {
        ++m_currentPage;
        return true;
    }
    return false;
}

bool GameListManager::PrevPage() {
    if (m_currentPage > 0) {
        --m_currentPage;
        return true;
    }
    return false;
}

bool GameListManager::ExportCsv(std::string_view accountName, std::string* outPath) const {
    std::string safeName{accountName};
    for (char& c : safeName) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    while (!safeName.empty() && (safeName.back() == '.' || safeName.back() == ' ')) {
        safeName.pop_back();
    }
    if (safeName.empty()) {
        safeName = "user";
    }

    std::string filename = "gameslist-" + safeName + ".csv";
    if (outPath) {
        *outPath = filename;
    }

    const std::filesystem::path filePath = Utf8Path(filename);
    const std::string filePathUtf8 = PathToUtf8(filePath);

    std::string csvData;
    csvData.reserve(m_games.size() * 64 + 64);
    // Write UTF-8 BOM
    csvData.append("\xEF\xBB\xBF", 3);
    csvData.append(std::format("{},{},{},{}\n", TR(MsgKey::CsvIndex), TR(MsgKey::CsvAppId), TR(MsgKey::CsvName), TR(MsgKey::CsvType)));

    for (size_t i = 0; i < m_games.size(); ++i) {
        const auto& g = m_games[i];
        csvData.append(std::format("{},{},{},{}\n", i + 1, g.appId, EscapeCsv(g.name),
                                   g.isShared ? TR(MsgKey::CsvShared) : TR(MsgKey::CsvOwned)));
    }

    if (!WriteTextFileAtomic(filePath, csvData)) {
        LOG_ERROR("GameList", MsgKey::LogGameListCreateCsvFailed, filePathUtf8);
        return false;
    }

    LOG_INFO("GameList", MsgKey::LogGameListExportCsvSuccess, MaskAccount(accountName), m_games.size());
    return true;
}

std::span<const OwnedGameInfo> GameListManager::GetPageItems(size_t pageIndex) const noexcept {
    const size_t total = m_games.size();
    const size_t startIdx = pageIndex * m_pageSize;
    if (startIdx >= total) return {};
    const size_t endIdx = (std::min)(startIdx + m_pageSize, total);
    return std::span<const OwnedGameInfo>(m_games.data() + startIdx, endIdx - startIdx);
}

} // namespace OST::ExtractTickets
