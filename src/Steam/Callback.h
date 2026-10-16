#pragma once

#include "Types.h"
#include "Enums.h"

#include <cstddef>
#include <type_traits>

#pragma pack(push, 8)

// ── ISteamUser callbacks (base = 100) ───────────────────────────────

constexpr int k_iSteamUserCallbacks = 100;

//-----------------------------------------------------------------------------
// Purpose: Result from RequestEncryptedAppTicket (async)
//-----------------------------------------------------------------------------
struct EncryptedAppTicketResponse_t
{
	enum { k_iCallback = k_iSteamUserCallbacks + 54 };

	EResult m_eResult;
};
static_assert(sizeof(EncryptedAppTicketResponse_t) == 4, "EncryptedAppTicketResponse_t size mismatch");
static_assert(alignof(EncryptedAppTicketResponse_t) == 4, "EncryptedAppTicketResponse_t alignment mismatch");
static_assert(std::is_standard_layout_v<EncryptedAppTicketResponse_t>, "EncryptedAppTicketResponse_t must be standard layout");

// ── CClientAppManager callbacks (base = 1020000) ────────────────────
constexpr int k_iClientAppManagerCallbacks = 1020000;

//-----------------------------------------------------------------------------
// Purpose: Broadcast when app licenses change (additions / removals / reload).
//          Sent by CClientAppManager after ProcessPendingLicenseUpdates.
//-----------------------------------------------------------------------------
struct AppLicensesChanged_t
{
	enum { k_iCallback = k_iClientAppManagerCallbacks + 94 };

	bool      m_bReloadAll;                // 0x00  — true = full library refresh
	bool      m_bIsFirstLoad;              // 0x01
	uint32    m_unRemainingPackets;         // 0x04
	uint32    m_unCount;                    // 0x08  — number of entries in m_rgAppsUpdated
	AppId_t   m_rgAppsUpdated[64];         // 0x0C  — batch of updated AppIds
	uint64    m_unAppsAdded;               // 0x110 — bitmask: bit N = m_rgAppsUpdated[N] was added
};
static_assert(sizeof(AppLicensesChanged_t) == 0x118, "AppLicensesChanged_t size mismatch");
static_assert(alignof(AppLicensesChanged_t) == 8, "AppLicensesChanged_t alignment mismatch");
static_assert(offsetof(AppLicensesChanged_t, m_rgAppsUpdated) == 0x0C, "m_rgAppsUpdated offset mismatch");
static_assert(offsetof(AppLicensesChanged_t, m_unAppsAdded) == 0x110, "m_unAppsAdded offset mismatch");
static_assert(std::is_standard_layout_v<AppLicensesChanged_t>, "AppLicensesChanged_t must be standard layout");

#pragma pack(pop)
