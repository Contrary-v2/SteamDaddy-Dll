#pragma once

#include <cstdint>
#include <string>

namespace OST::ExtractTickets {

class OnlineSession {
public:
    // Starts the interactive online mode (Level 2 -> Level 3 flow)
    // Returns 0 on normal exit, non-zero on critical error
    static int RunInteractive();

    // Runs silent, non-interactive online extraction of a single AppID using cached DPAPI credentials
    // If accountName is empty:
    //   - Automatically picks the account if exactly one account is cached, or latest active account
    // Returns 0 on success, non-zero on error
    static int RunSilent(uint32_t appId, const std::string& accountName = "", bool forceEticket = false);

private:
    // Level 2: Account Selection and Management Menu
    static void RunAccountSelectionMenu();

    // Level 3: Inside a logged-in account (games list, pagination, extraction)
    static void RunInSessionExtraction(
        const std::string& accountName,
        uint64_t steamId,
        const std::string& refreshToken,
        const std::string& accessToken);
};

} // namespace OST::ExtractTickets
