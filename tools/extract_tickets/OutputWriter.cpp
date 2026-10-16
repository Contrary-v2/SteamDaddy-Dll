#include "OutputWriter.h"
#include "I18n.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>

namespace OST::ExtractTickets {

namespace {
std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket || ticket->empty()) return std::format("{}:null\n", name);
    return std::format("{}({}bytes):{}\n", name, ticket->size(), ToHexString(*ticket));
}

class EmitTracker {
public:
    [[nodiscard]] bool TryEmitAppId(uint32_t id) noexcept {
        return m_emittedAppIds.insert(id).second;
    }
    [[nodiscard]] bool TryEmitManifest(uint32_t depotId) noexcept {
        return m_emittedManifests.insert(depotId).second;
    }
    [[nodiscard]] bool TryEmitToken(uint32_t id) noexcept {
        return m_emittedTokens.insert(id).second;
    }

private:
    std::unordered_set<uint32_t> m_emittedAppIds;
    std::unordered_set<uint32_t> m_emittedManifests;
    std::unordered_set<uint32_t> m_emittedTokens;
};

} // namespace

bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens,
                  bool isOnlineMode) {
    const std::string dir{std::to_string(appId)};
    std::filesystem::path dirPath = Utf8Path(dir);
    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutCreateDirFailed, dir, ec.message()) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutCreateDirFailed, dir, ec.message());
        return false;
    }

    bool ok{true};
    if (ownership && !ownership->empty()) ok = WriteBinaryFile(dirPath / "appticket.bin", *ownership) && ok;
    if (encrypted && !encrypted->empty()) ok = WriteBinaryFile(dirPath / "eticket.bin", *encrypted) && ok;

    // Copy manifest files (.manifest) if found in depotcache
    std::vector<std::string> copiedManifests;
    copiedManifests.reserve(depotKeys.size());
    for (const auto& dk : depotKeys) {
        if (!dk.manifestFilePath.empty()) {
            std::filesystem::path srcPath = Utf8Path(dk.manifestFilePath);
            std::string fname = PathToUtf8(srcPath.filename());
            if (std::ranges::find(copiedManifests, fname) == copiedManifests.end()) {
                std::filesystem::path destPath = dirPath / fname;
                std::error_code copyEc;
                if (std::filesystem::equivalent(srcPath, destPath, copyEc)) {
                    copiedManifests.push_back(fname);
                } else if (std::filesystem::copy_file(srcPath, destPath, std::filesystem::copy_options::overwrite_existing, copyEc)) {
                    copiedManifests.push_back(fname);
                } else {
                    if (std::filesystem::exists(destPath, copyEc)) {
                        copiedManifests.push_back(fname);
                    } else {
                        if (!TuiEngine::IsActive()) {
                            std::cerr << TR_FMT(MsgKey::OutCopyManifestFailed, fname, copyEc.message()) << "\n";
                        }
                        LOG_WARN("OutputWriter", MsgKey::LogOutCopyManifestFailed, fname, copyEc.message());
                    }
                }
            }
        }
    }

    uint64_t baseToken = 0;
    auto baseTokIt = appTokens.find(appId);
    if (baseTokIt != appTokens.end()) {
        baseToken = baseTokIt->second;
    }

    std::map<uint32_t, uint64_t> relevantTokens;
    if (baseToken != 0) {
        relevantTokens[appId] = baseToken;
    }
    for (const auto& dlc : dlcs) {
        auto it = appTokens.find(dlc.dlcId);
        if (it != appTokens.end() && it->second != 0) {
            relevantTokens[dlc.dlcId] = it->second;
        }
    }

    // Build tickets.txt summary
    std::string text;
    text.reserve(2048);
    std::format_to(std::back_inserter(text), "appid:{}\n", appId);
    std::unordered_set<uint32_t> emittedTxtDlc;
    for (const auto& dlc : dlcs) {
        if (emittedTxtDlc.insert(dlc.dlcId).second) {
            if (!dlc.name.empty()) {
                std::format_to(std::back_inserter(text), "dlc({}):{}\n", dlc.dlcId, SanitizeComment(dlc.name));
            } else {
                std::format_to(std::back_inserter(text), "dlc({})\n", dlc.dlcId);
            }
        }
    }
    std::unordered_set<uint32_t> emittedTxtKeys;
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty() && emittedTxtKeys.insert(dk.depotId).second) {
            std::format_to(std::back_inserter(text), "depotkey({}):{}\n", dk.depotId, dk.hexKey);
        }
    }
    std::unordered_set<uint32_t> emittedTxtMans;
    for (const auto& dk : depotKeys) {
        if (IsValidManifestId(dk.manifestId) && emittedTxtMans.insert(dk.depotId).second) {
            std::format_to(std::back_inserter(text), "manifest({}):{}\n", dk.depotId, dk.manifestId);
        }
    }
    if (baseToken != 0) {
        std::format_to(std::back_inserter(text), "token({}):{}\n", appId, baseToken);
    } else {
        std::format_to(std::back_inserter(text), "token({}):null\n", appId);
    }
    for (const auto& [tId, tVal] : relevantTokens) {
        if (tId != appId) {
            std::format_to(std::back_inserter(text), "token({}):{}\n", tId, tVal);
        }
    }
    text += TicketLine("appticket", ownership);
    text += TicketLine("eticket", encrypted);

    const std::filesystem::path textPath = dirPath / "tickets.txt";
    if (!WriteTextFileAtomic(textPath, text, false)) {
        const std::string textPathUtf8 = PathToUtf8(textPath);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, textPathUtf8) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutWriteTicketsTxtFailed, textPathUtf8);
        return false;
    }

    // Generate ready-to-use Lua script
    std::string luaText;
    luaText.reserve(8192);
    std::format_to(std::back_inserter(luaText), "{}{}\n\n", TR(MsgKey::LuaHeader), appId);

    std::unordered_set<uint32_t> ownedDlcIdSet;
    for (const auto& dlc : dlcs) {
        ownedDlcIdSet.insert(dlc.dlcId);
    }

    const std::unordered_set<std::string> copiedManifestSet(copiedManifests.begin(), copiedManifests.end());
    auto hasManifestOnDisk = [&](uint32_t depotId, const std::string& manifestId) -> bool {
        std::string fname = std::format("{}_{}.manifest", depotId, manifestId);
        if (copiedManifestSet.contains(fname)) return true;
        std::filesystem::path p = dirPath / fname;
        std::error_code diskEc;
        return std::filesystem::exists(p, diskEc);
    };

    EmitTracker emitTracker;

    // 1. Base Game Unlocks (AppID and base game depots)
    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaBaseGame));
    const DepotKeyInfo* baseAppDk = nullptr;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId) {
            baseAppDk = &dk;
            break;
        }
    }

    if (emitTracker.TryEmitAppId(appId)) {
        if (baseAppDk && !baseAppDk->hexKey.empty()) {
            std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", appId, baseAppDk->hexKey);
        } else {
            std::format_to(std::back_inserter(luaText), "addappid({})\n", appId);
        }
    }

    if (baseToken != 0 && emitTracker.TryEmitToken(appId)) {
        std::format_to(std::back_inserter(luaText), "addtoken({}, \"{}\")\n", appId, baseToken);
    }

    for (const auto& dk : depotKeys) {
        const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
        if (dk.depotId != appId && !isDlcDepot) {
            if (!dk.hexKey.empty() && emitTracker.TryEmitAppId(dk.depotId)) {
                std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", dk.depotId, dk.hexKey);
            }
        }
    }

    // 2. Base Game Manifests (placed right under Base Game)
    bool hasBaseManifests = false;
    if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
        hasBaseManifests = true;
    }
    if (!hasBaseManifests) {
        for (const auto& dk : depotKeys) {
            const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                    (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
            if (dk.depotId != appId && !isDlcDepot) {
                if (IsValidManifestId(dk.manifestId)) {
                    hasBaseManifests = true;
                    break;
                }
            }
        }
    }

    if (hasBaseManifests) {
        std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaBaseManifests));
        if (baseAppDk && IsValidManifestId(baseAppDk->manifestId) && emitTracker.TryEmitManifest(appId)) {
            if (hasManifestOnDisk(appId, baseAppDk->manifestId)) {
                std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", appId, baseAppDk->manifestId);
            } else {
                std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", appId, baseAppDk->manifestId, TR(MsgKey::LuaManifestMissing));
            }
        }
        for (const auto& dk : depotKeys) {
            const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                    (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
            if (dk.depotId != appId && !isDlcDepot) {
                if (IsValidManifestId(dk.manifestId) && emitTracker.TryEmitManifest(dk.depotId)) {
                    if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                        std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", dk.depotId, dk.manifestId);
                    } else {
                        std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", dk.depotId, dk.manifestId, TR(MsgKey::LuaManifestMissing));
                    }
                }
            }
        }
    }

    // 3. Owned DLCs Unlocks
    if (!dlcs.empty()) {
        struct DlcDepotData {
            const DepotKeyInfo* dlcDk = nullptr;
            std::vector<const DepotKeyInfo*> subDepots;
            std::vector<const DepotKeyInfo*> associatedDepots;
        };
        std::vector<DlcDepotData> dlcDepotList(dlcs.size());
        bool hasDlcManifests = false;

        std::unordered_map<uint32_t, const DepotKeyInfo*> primaryDepots;
        std::unordered_map<uint32_t, std::vector<const DepotKeyInfo*>> subDepotsMap;
        primaryDepots.reserve(depotKeys.size());
        for (const auto& dk : depotKeys) {
            primaryDepots[dk.depotId] = &dk;
            if (dk.dlcId != 0 && dk.dlcId != dk.depotId) {
                subDepotsMap[dk.dlcId].push_back(&dk);
            }
        }

        // Single pass over dlcs to build indexed association table in strictly O(N + M) time
        for (size_t i = 0; i < dlcs.size(); ++i) {
            auto& data = dlcDepotList[i];
            const uint32_t dlcId = dlcs[i].dlcId;
            if (auto it = primaryDepots.find(dlcId); it != primaryDepots.end()) {
                data.dlcDk = it->second;
                data.associatedDepots.push_back(it->second);
            }
            if (auto it = subDepotsMap.find(dlcId); it != subDepotsMap.end()) {
                data.subDepots = it->second;
                data.associatedDepots.insert(data.associatedDepots.end(), it->second.begin(), it->second.end());
            }
            if (!hasDlcManifests) {
                for (const auto* dk : data.associatedDepots) {
                    if (IsValidManifestId(dk->manifestId)) {
                        hasDlcManifests = true;
                        break;
                    }
                }
            }
        }

        std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaOwnedDlcs));
        for (size_t i = 0; i < dlcs.size(); ++i) {
            const auto& dlc = dlcs[i];
            const auto& data = dlcDepotList[i];

            bool isDepotIncomplete = false;
            bool hasPhysicalDepot = false;
            for (const auto* dk : data.associatedDepots) {
                if (IsValidManifestId(dk->manifestId)) {
                    hasPhysicalDepot = true;
                    const bool manifestPresent = hasManifestOnDisk(dk->depotId, dk->manifestId);
                    if (!manifestPresent) {
                        isDepotIncomplete = true;
                        break;
                    }
                }
            }
            if (!hasPhysicalDepot) {
                isDepotIncomplete = false; // Pure logical DLC: zero physical files on CDN, always active
            }

            // 纵深防御：对于既无物理实体又无公开名称的孤立空项直接跳过
            if (!hasPhysicalDepot && dlc.name.empty()) {
                LOG_DEBUG("OutputWriter", MsgKey::LogOutDroppedOrphanGhostDlc, dlc.dlcId);
                continue;
            }

            const std::string_view prefix = isDepotIncomplete ? "-- " : "";
            const std::string reasonSuffix = isDepotIncomplete ? (" -- " + std::string(TR(MsgKey::LuaDlcDepotMissing))) : "";

            if (emitTracker.TryEmitAppId(dlc.dlcId)) {
                if (data.dlcDk && !data.dlcDk->hexKey.empty()) {
                    std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\")", prefix, dlc.dlcId, data.dlcDk->hexKey);
                } else {
                    std::format_to(std::back_inserter(luaText), "{}addappid({}, 1)", prefix, dlc.dlcId);
                }

                if (!dlc.name.empty()) {
                    luaText += " -- " + SanitizeComment(dlc.name);
                }
                luaText += reasonSuffix + "\n";
            } else {
                LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedAppId, dlc.dlcId);
            }

            auto dlcTokIt = appTokens.find(dlc.dlcId);
            if (dlcTokIt != appTokens.end() && dlcTokIt->second != 0 && emitTracker.TryEmitToken(dlc.dlcId)) {
                std::format_to(std::back_inserter(luaText), "{}addtoken({}, \"{}\")\n", prefix, dlc.dlcId, dlcTokIt->second);
            }

            // Any subdepots of this DLC with keys
            for (const auto* subDk : data.subDepots) {
                if (!subDk->hexKey.empty()) {
                    if (emitTracker.TryEmitAppId(subDk->depotId)) {
                        std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\"){}\n", prefix, subDk->depotId, subDk->hexKey, reasonSuffix);
                    } else {
                        LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedAppId, subDk->depotId);
                    }
                }
            }
        }

        // 4. DLC Manifests (placed right under DLC unlocks)
        if (hasDlcManifests) {
            std::format_to(std::back_inserter(luaText), "\n{}\n", TR(MsgKey::LuaDlcManifests));
            for (size_t i = 0; i < dlcs.size(); ++i) {
                const auto& dlc = dlcs[i];
                const auto& data = dlcDepotList[i];

                // DLC itself manifest
                if (data.dlcDk && IsValidManifestId(data.dlcDk->manifestId)) {
                    if (emitTracker.TryEmitManifest(data.dlcDk->depotId)) {
                        const bool onDisk = hasManifestOnDisk(data.dlcDk->depotId, data.dlcDk->manifestId);
                        if (onDisk) {
                            std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")", data.dlcDk->depotId, data.dlcDk->manifestId);
                        } else {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}", data.dlcDk->depotId, data.dlcDk->manifestId, TR(MsgKey::LuaManifestMissing));
                        }
                        if (!dlc.name.empty()) {
                            luaText += " -- " + SanitizeComment(dlc.name);
                        }
                        luaText += "\n";
                    } else {
                        LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedManifest, data.dlcDk->depotId);
                    }
                }

                // Any subdepots of this DLC with manifests
                for (const auto* subDk : data.subDepots) {
                    if (IsValidManifestId(subDk->manifestId)) {
                        if (emitTracker.TryEmitManifest(subDk->depotId)) {
                            const bool onDisk = hasManifestOnDisk(subDk->depotId, subDk->manifestId);
                            if (onDisk) {
                                std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", subDk->depotId, subDk->manifestId);
                            } else {
                                std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", subDk->depotId, subDk->manifestId, TR(MsgKey::LuaManifestMissing));
                            }
                        } else {
                            LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedManifest, subDk->depotId);
                        }
                    }
                }
            }
        }
    }

    // 5. App Tickets (at the very end of the file)
    luaText += "\n";
    const bool hasOwnership = (ownership && !ownership->empty());
    const bool hasEncrypted = (encrypted && !encrypted->empty());

    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaAppTicket));
    if (hasOwnership) {
        std::format_to(std::back_inserter(luaText), "setAppTicket({}, \"{}\")\n\n",
                       appId,
                       ToHexString(*ownership));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setAppTicket({}, \"null\") -- {}\n\n", appId, TR(MsgKey::LuaAppTicketMissing));
    }

    std::format_to(std::back_inserter(luaText), "{}\n", TR(MsgKey::LuaETicket));
    if (hasEncrypted) {
        std::format_to(std::back_inserter(luaText), "setETicket({}, \"{}\")\n", appId, ToHexString(*encrypted));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setETicket({}, \"null\") -- {}\n", appId, TR(MsgKey::LuaETicketNote));
    }

    const std::filesystem::path luaPath = dirPath / (std::to_string(appId) + ".lua");
    if (!WriteTextFileAtomic(luaPath, luaText, true)) {
        const std::string luaPathUtf8 = PathToUtf8(luaPath);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, luaPathUtf8) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutWriteLuaFailed, luaPathUtf8);
        ok = false;
    }

    const std::string luaPathUtf8 = PathToUtf8(luaPath);

    if (!TuiEngine::IsActive()) {
        std::string fileList = std::to_string(appId) + ".lua, tickets.txt";
        if (ownership && !ownership->empty()) fileList += ", appticket.bin";
        if (encrypted && !encrypted->empty()) fileList += ", eticket.bin";
        for (const auto& mName : copiedManifests) {
            fileList += ", " + mName;
        }
        std::cout << TR_FMT(MsgKey::OutWroteSummary, dir, fileList) << "\n";

        if (!dlcs.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedDlcs, dlcs.size()) << "\n";
            for (const auto& dlc : dlcs) {
                std::cout << "       DLC " << dlc.dlcId;
                if (!dlc.name.empty()) {
                    std::cout << ": " << dlc.name;
                }
                std::cout << "\n";
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoDlcs, appId) << "\n";
        }

        const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
            return !dk.hexKey.empty();
        });
        if (keyCount > 0) {
            std::cout << TR_FMT(MsgKey::OutExtractedDepotKeys, keyCount) << "\n";
            for (const auto& dk : depotKeys) {
                if (!dk.hexKey.empty()) {
                    std::cout << "       Depot " << dk.depotId << ": " << dk.hexKey << "\n";
                }
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoDepotKeys, appId) << "\n";
            std::cout << TR(MsgKey::OutDepotKeysTip) << "\n";
        }

        if (!copiedManifests.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedManifests, copiedManifests.size()) << "\n";
            for (const auto& mName : copiedManifests) {
                std::cout << "       " << mName << "\n";
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoManifests, appId) << "\n";
        }

        if (!relevantTokens.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedTokens, relevantTokens.size()) << "\n";
            for (const auto& [tId, tVal] : relevantTokens) {
                std::cout << "       AppID " << tId << ": " << tVal << "\n";
            }
        } else if (isOnlineMode) {
            std::cout << TR(MsgKey::OutPublicProductTokenZero) << "\n";
        } else {
            std::cout << TR_FMT(MsgKey::OutNoAccessTokenLocal, appId) << "\n";
        }

        std::cout << TR_FMT(MsgKey::OutConfigSaved, luaPathUtf8) << "\n";
    }

    const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
        return !dk.hexKey.empty();
    });
    LOG_INFO("OutputWriter", MsgKey::LogOutFilesWritten,
             dir, dlcs.size(), keyCount, copiedManifests.size(), relevantTokens.size());
    LOG_INFO("OutputWriter", MsgKey::LogOutLuaSaved, luaPathUtf8);
    return ok;
}

} // namespace OST::ExtractTickets
