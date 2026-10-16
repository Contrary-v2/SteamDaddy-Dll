#pragma once

// Layout of the in-memory structures we manipulate from hooks. Most of
// these mirror Valve internal classes (CUtlVector, PackageInfo, etc.) and
// are kept in lockstep with steamclient(64).dll.

#include "Types.h"
#include "Enums.h"


#include <atomic>
#include <compare>
#include <cstddef>
#include <cstring>
#include <format>
#include <ostream>
#include <string>
#include <type_traits>

template<typename T>
struct CUtlMemory{
	T* m_pMemory;
	uint32 m_nAllocationCount;
	uint32 m_nGrowSize;
};
static_assert(sizeof(CUtlMemory<void*>) == 16, "CUtlMemory size mismatch");
static_assert(alignof(CUtlMemory<void*>) == 8, "CUtlMemory alignment mismatch");
static_assert(std::is_standard_layout_v<CUtlMemory<void*>>, "CUtlMemory must be standard layout");

template<typename T>
struct CUtlVector {
	CUtlMemory<T> m_Memory;
	uint32 m_Size;

	// Swap with last element and shrink. Does not preserve order.
	void FastRemove(uint32 elem) noexcept {
		if (elem < m_Size) {
			if (elem != m_Size - 1)
				m_Memory.m_pMemory[elem] = m_Memory.m_pMemory[m_Size - 1];
			--m_Size;
		}
	}

	bool FindAndFastRemove(const T& src) noexcept {
		for (uint32 i = 0; i < m_Size; ++i) {
			if (m_Memory.m_pMemory[i] == src) {
				FastRemove(i);
				return true;
			}
		}
		return false;
	}
};
static_assert(sizeof(CUtlVector<void*>) == 24, "CUtlVector size mismatch");
static_assert(alignof(CUtlVector<void*>) == 8, "CUtlVector alignment mismatch");
static_assert(std::is_standard_layout_v<CUtlVector<void*>>, "CUtlVector must be standard layout");

struct CUtlBuffer{
	CUtlMemory<uint8> m_Memory;
	int32 m_Get;
	int32 m_Put;
	int32 m_nOffset;
	int32 m_flags;

	typedef bool (CUtlBuffer::*UtlBufferOverflowFunc_t)( int32 nSize );
	UtlBufferOverflowFunc_t m_GetOverflowFunc;
	UtlBufferOverflowFunc_t m_PutOverflowFunc;

	// Direct base/size helpers — match the names the original Valve
	// CUtlBuffer exposes so call sites read naturally.
	[[nodiscard]] uint8* Base() noexcept             { return m_Memory.m_pMemory; }
	[[nodiscard]] const uint8* Base() const noexcept { return m_Memory.m_pMemory; }
	[[nodiscard]] int32 TellPut() const noexcept     { return m_Put; }
	[[nodiscard]] int32 TellGet() const noexcept     { return m_Get; }

	[[nodiscard]] uint32 Size() const noexcept        { return m_Memory.m_nAllocationCount; }
	[[nodiscard]] uint32 Capacity() const noexcept    { return m_Memory.m_nAllocationCount; }

	// Debug helper
	std::string DebugString() const {
		return std::format("m_Memory:0x{:X} m_AllocCnt:{} m_Grow:{} m_Get:{} m_Put:{} m_nOffset:{} m_flags:{}",
			reinterpret_cast<uintptr_t>(m_Memory.m_pMemory),
			m_Memory.m_nAllocationCount, m_Memory.m_nGrowSize,
			m_Get, m_Put, m_nOffset, m_flags);
	}
};
static_assert(sizeof(CUtlBuffer) == 48, "CUtlBuffer size mismatch");
static_assert(alignof(CUtlBuffer) == 8, "CUtlBuffer alignment mismatch");
static_assert(offsetof(CUtlBuffer, m_Get) == 0x10, "m_Get offset mismatch");
static_assert(offsetof(CUtlBuffer, m_Put) == 0x14, "m_Put offset mismatch");
static_assert(offsetof(CUtlBuffer, m_GetOverflowFunc) == 0x20, "m_GetOverflowFunc offset mismatch");
static_assert(std::is_standard_layout_v<CUtlBuffer>, "CUtlBuffer must be standard layout");

struct PackageInfo
{
	AppId_t PackageId;
	int32 ChangeNumber;
	uint64 PICS_token;
	BillingType BillingType;
	ELicenseType LicenseType;
	EPackageStatus Status;
	byte SHA_1_Hash[20];
	void* pPackageInfoNodeBegin;
	void* pExtendNodeBegin;

	CUtlVector<AppId_t> AppIdVec;
	CUtlVector<DepotId_t> DepotIdVec;
};
static_assert(sizeof(PackageInfo) == 0x70, "PackageInfo size mismatch");
static_assert(alignof(PackageInfo) == 8, "PackageInfo alignment mismatch");
static_assert(offsetof(PackageInfo, PICS_token) == 0x08, "PICS_token offset mismatch");
static_assert(offsetof(PackageInfo, SHA_1_Hash) == 0x1C, "SHA_1_Hash offset mismatch");
static_assert(offsetof(PackageInfo, pPackageInfoNodeBegin) == 0x30, "pPackageInfoNodeBegin offset mismatch");
static_assert(offsetof(PackageInfo, AppIdVec) == 0x40, "AppIdVec offset mismatch");
static_assert(offsetof(PackageInfo, DepotIdVec) == 0x58, "DepotIdVec offset mismatch");
static_assert(std::is_standard_layout_v<PackageInfo>, "PackageInfo must be standard layout");

struct AppOwnership
{
	PackageId_t PackageId;
	EAppReleaseState ReleaseState; 
	AccountID_t SteamId32; 
	AppId_t MasterSubscriptionAppID; 
	uint32 TrialSeconds; 
	uint32 ExistInPackageNums; 
	char PurchaseCountryCode[4];
	uint32 TimeStamp;
	uint32 TimeExpire;
	bool bOwnsLicense;
	bool bLicenseExpired;
	bool bIsPermanent;
	bool bLowViolence;
	bool bFreeLicense;
	bool bRegionRestricted;
	bool bFromFreeWeekend;
	bool bLicenseLocked;
	bool bLicensePending;
	bool bRetailLicense;
	bool bAutoGrant;
	bool bLicensePermanent;
	bool bGuestPass;
	bool bBorrowed;
	bool bAnySiteLicense;
	bool bAllSiteLicenses;
	bool bAllActivationRequired;
	bool bFamilyShared;
	uint8_t _safetyPadding[64]{};
};
static_assert(sizeof(AppOwnership) == 120, "AppOwnership must be 120 bytes");
static_assert(alignof(AppOwnership) == 4, "AppOwnership alignment mismatch");
static_assert(offsetof(AppOwnership, PurchaseCountryCode) == 0x18, "PurchaseCountryCode offset mismatch");
static_assert(offsetof(AppOwnership, bOwnsLicense) == 0x24, "bOwnsLicense offset mismatch");
static_assert(offsetof(AppOwnership, _safetyPadding) == 0x36, "_safetyPadding offset mismatch");
static_assert(std::is_standard_layout_v<AppOwnership>, "AppOwnership must be standard layout");

// Single depot manifest entry (0x20 bytes) produced by BuildDepotDependency.
struct DepotEntry
{
	DepotId_t DepotId;        // 0x00
	AppId_t   AppId;          // 0x04
	GID_t     ManifestGid;    // 0x08 — from depots/<id>/manifests/<branch>/gid
	uint64    ManifestSize;   // 0x10 — from depots/<id>/manifests/<branch>/size
	AppId_t   DlcAppId;       // 0x18 — associated DLC AppID, 0 if none
	uint8     LcsRequired;    // 0x1C — branches/<branch>/lcsrequired
	uint8     bNotNewTarget;  // 0x1D — carried over from active list (not newly activated this call)
	uint8     SharedInstall;  // 0x1E — sharedinstall / depotfromapp redirect
	uint8     Padding;        // 0x1F
};
static_assert(sizeof(DepotEntry) == 0x20, "DepotEntry must be 32 bytes");
static_assert(alignof(DepotEntry) == 8, "DepotEntry alignment mismatch");
static_assert(offsetof(DepotEntry, ManifestGid) == 0x08, "ManifestGid offset mismatch");
static_assert(offsetof(DepotEntry, ManifestSize) == 0x10, "ManifestSize offset mismatch");
static_assert(offsetof(DepotEntry, DlcAppId) == 0x18, "DlcAppId offset mismatch");
static_assert(std::is_standard_layout_v<DepotEntry>, "DepotEntry must be standard layout");
static_assert(std::is_trivially_copyable_v<DepotEntry>, "DepotEntry must be trivially copyable");

struct KeyValues
{
	union                                   // +0x00 (8B) — value storage OR first child
	{
		KeyValues*      m_pSub;             // TYPE_NONE  → pointer to first child
		char*           m_sValue;           // TYPE_STRING
		wchar_t*        m_wsValue;          // TYPE_WSTRING
		int             m_iValue;           // TYPE_INT
		float           m_flValue;          // TYPE_FLOAT
		void*           m_pValue;           // TYPE_PTR
		uint64			m_ullValue;         // TYPE_UINT64
		int64           m_llValue;          // TYPE_INT64
		byte   			m_Color[8];         // TYPE_COLOR
	};

	KeyValues*          m_pChain;           // +0x08 (8B) — chained KeyValues for fallback lookup

	// +0x10 (4B) — packed bitfield
	//   bit[0:24]  m_iKeyName              (25 bits)  key symbol assigned by KeyValuesSystem
	//   bit[25:28] m_iDataType             (4 bits)   value type enum (see EKeyValuesType)
	//   bit[29]    m_bHasEscapeSequences              escape sequences enabled during parse
	//   bit[30]    m_bEvaluateConditionals            conditional blocks evaluated during parse
	//   bit[31]    m_bAllocatedValue                  value allocated on heap (must deref offset 0)
	union
	{
		struct
		{
			unsigned int m_iKeyName              : 25;
			unsigned int m_iDataType             : 4;
			unsigned int m_bHasEscapeSequences   : 1;
			unsigned int m_bEvaluateConditionals : 1;
			unsigned int m_bAllocatedValue       : 1;
		};
		unsigned int m_iPackedKeyAndType;    // raw DWORD access
	};

	unsigned int        m_unFlags;          // +0x14 (4B) — additional flags

	KeyValues*          m_pPeer;            // +0x18 (8B) — next sibling in linked list

};
static_assert(sizeof(KeyValues) == 0x20, "KeyValues must be 32 bytes");
static_assert(alignof(KeyValues) == 8, "KeyValues alignment mismatch");
static_assert(offsetof(KeyValues, m_pChain) == 0x08, "m_pChain offset mismatch");
static_assert(offsetof(KeyValues, m_unFlags) == 0x14, "m_unFlags offset mismatch");
static_assert(offsetof(KeyValues, m_pPeer) == 0x18, "m_pPeer offset mismatch");

// ============================================================
// IKeyValuesSystem
//   Exported as "KeyValuesSystemSteam" (ord 103) from vstdlib_s64.dll
// ============================================================
struct IKeyValuesSystem {
	// vtable[0] (+0x00) — records max KeyValues size for memory pooling
	virtual void        RegisterSizeofKeyValues(int size) = 0;

	// vtable[1] (+0x08) — hash string → return symbol (create if not found)
	//   Symbol = (pool_index << 15) | byte_offset_within_pool
	//   Returns 0 if name is null/empty and bCreate=false
	virtual int         GetSymbolForString(const char* name, bool bCreate) = 0;

	// vtable[2] (+0x10) — O(1) reverse lookup: symbol → string pointer
	//   Returns "" (empty string) for invalid/zero symbols
	virtual const char* GetStringForSymbol(int symbol) = 0;

	// vtable[3..11] — Alloc/Free memory, leak tracking, file cache, etc.
	//   (not needed for depot override, omitted)

	// ── helpers ──
	int  GetSymbol(const char* name)        { return GetSymbolForString(name, false); }
	int  GetOrCreateSymbol(const char* name) { return GetSymbolForString(name, true); }
	const char* GetKeyName(int symbol)       { return GetStringForSymbol(symbol); }
};
using KeyValuesSystemSteam_t = IKeyValuesSystem* (*)();

// Legacy third-party CD-key messages (k_EMsgClientGetLegacyGameKey / Response).
// The request asks Steam for the per-account key for a title; payload is just the
// struct below; the response's key payload (m_cchKey bytes) trails the struct.
#pragma pack(push,1)
struct MsgClientGetLegacyGameKey
{
	uint32 m_unAppId;
};

struct MsgClientGetLegacyGameKeyResponse
{
	uint32  m_unAppId;
	EResult m_eResult;
	uint32  m_cchKey;   // length of the CD-key payload that follows this struct
};
#pragma pack(pop)

namespace NetPacketLayout {
	inline std::atomic<uint32> s_dataOffset{16};
	inline std::atomic<uint32> s_sizeOffset{24};
	inline std::atomic<bool>   s_resolved{false};

	inline void Reset() noexcept {
		s_dataOffset.store(16, std::memory_order_relaxed);
		s_sizeOffset.store(24, std::memory_order_relaxed);
		s_resolved.store(false, std::memory_order_release);
	}
}

struct CNetPacket
{
	// Dynamic offset-aware resilient safe accessors
	[[nodiscard]] uint8* PubData() const noexcept {
		return PubData(NetPacketLayout::s_dataOffset.load(std::memory_order_relaxed));
	}
	void SetPubData(uint8* data) noexcept {
		SetPubData(data, NetPacketLayout::s_dataOffset.load(std::memory_order_relaxed));
	}
	[[nodiscard]] uint32 CubData() const noexcept {
		return CubData(NetPacketLayout::s_sizeOffset.load(std::memory_order_relaxed));
	}
	void SetCubData(uint32 size) noexcept {
		SetCubData(size, NetPacketLayout::s_sizeOffset.load(std::memory_order_relaxed));
	}

	[[nodiscard]] uint8* PubData(uint32 dataOff) const noexcept {
		return *reinterpret_cast<uint8* const*>(reinterpret_cast<const uint8*>(this) + dataOff);
	}
	void SetPubData(uint8* data, uint32 dataOff) noexcept {
		*reinterpret_cast<uint8**>(reinterpret_cast<uint8*>(this) + dataOff) = data;
	}
	[[nodiscard]] uint32 CubData(uint32 sizeOff) const noexcept {
		return *reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(this) + sizeOff);
	}
	void SetCubData(uint32 size, uint32 sizeOff) noexcept {
		*reinterpret_cast<uint32*>(reinterpret_cast<uint8*>(this) + sizeOff) = size;
	}

	// Ergonomic aliases
	[[nodiscard]] uint8* GetData() const noexcept { return PubData(); }
	void SetData(uint8* data) noexcept { SetPubData(data); }
	[[nodiscard]] uint32 GetSize() const noexcept { return CubData(); }
	void SetSize(uint32 size) noexcept { SetCubData(size); }
};

#pragma pack(push, 1)
struct MsgHdr
{
	EMsg eMsg;               // actual_eMsg | 0x80000000
	uint32 headerLength;
};
#pragma pack(pop)
static_assert(sizeof(MsgHdr) == 8, "MsgHdr must be 8 bytes");
static_assert(alignof(MsgHdr) == 1, "MsgHdr alignment mismatch");
static_assert(offsetof(MsgHdr, headerLength) == 4, "headerLength offset mismatch");
static_assert(std::is_standard_layout_v<MsgHdr>, "MsgHdr must be standard layout");
static_assert(std::is_trivially_copyable_v<MsgHdr>, "MsgHdr must be trivially copyable");

// High bit of eMsg discriminates protobuf vs extended header.
constexpr uint32 kMsgHdrProtoFlag = 0x80000000;

#pragma pack(push,1)
struct ExtendedMsgHdr
{
	EMsg eMsg;
	uint8 m_nCubHdr;
	uint16 m_nHdrVersion;
	JobID_t m_JobIDTarget;
	JobID_t m_JobIDSource;
	uint8 m_nHdrCanary;
	uint64 m_ulSteamID;
	int32 m_nSessionID;
};
#pragma pack(pop)
static_assert(sizeof(ExtendedMsgHdr) == 36, "ExtendedMsgHdr must be 36 bytes");
static_assert(alignof(ExtendedMsgHdr) == 1, "ExtendedMsgHdr must be 1-byte aligned");
static_assert(offsetof(ExtendedMsgHdr, m_JobIDTarget) == 0x07, "m_JobIDTarget offset mismatch");
static_assert(offsetof(ExtendedMsgHdr, m_JobIDSource) == 0x0F, "m_JobIDSource offset mismatch");
static_assert(offsetof(ExtendedMsgHdr, m_ulSteamID) == 0x18, "m_ulSteamID offset mismatch");
static_assert(std::is_standard_layout_v<ExtendedMsgHdr>, "ExtendedMsgHdr must be standard layout");
static_assert(std::is_trivially_copyable_v<ExtendedMsgHdr>, "ExtendedMsgHdr must be trivially copyable");

// ── CPipeClient ────────────────────────────────────────────
struct CPipeClient {
    void*    m_pServer;         // +0
    void*    m_pClient;         // +8
    HSteamPipe  m_hSteamPipe;      // +16
    uint8    _pad0[12];         // +20
    PID_t    m_clientPID;       // +32
    uint8    _pad1[4];          // +36
    char*    m_szProcessName;   // +40
    uint8    _pad2[80];         // +48
    int32    m_nActiveRefs;     // +128
    uint8    _pad3[188];        // +132
    void*    m_pLocalIPCServer; // +320
    bool     m_bInProcess;      // +328
    uint8    _pad4[31];         // +329

	std::string DebugString() const {
		return std::format("pipe=0x{:08X} pid={} proc={} ",
			m_hSteamPipe, m_clientPID, m_szProcessName ? m_szProcessName : "?");
	}
};
static_assert(sizeof(CPipeClient) == 360, "CPipeClient must be 360 bytes");
static_assert(alignof(CPipeClient) == 8, "CPipeClient alignment mismatch");
static_assert(offsetof(CPipeClient, m_hSteamPipe) == 0x10, "m_hSteamPipe offset mismatch");
static_assert(offsetof(CPipeClient, m_clientPID) == 0x20, "m_clientPID offset mismatch");
static_assert(offsetof(CPipeClient, m_szProcessName) == 0x28, "m_szProcessName offset mismatch");
static_assert(offsetof(CPipeClient, m_nActiveRefs) == 0x80, "m_nActiveRefs offset mismatch");
static_assert(offsetof(CPipeClient, m_pLocalIPCServer) == 0x140, "m_pLocalIPCServer offset mismatch");
static_assert(offsetof(CPipeClient, m_bInProcess) == 0x148, "m_bInProcess offset mismatch");
static_assert(std::is_standard_layout_v<CPipeClient>, "CPipeClient must be standard layout");

struct CGameID{
	enum EGameIDType
	{
		k_EGameIDTypeApp		= 0,
		k_EGameIDTypeGameMod	= 1,
		k_EGameIDTypeShortcut	= 2,
		k_EGameIDTypeP2P		= 3,
	};

	[[nodiscard]] constexpr bool IsSteamApp() const noexcept
	{
		return ( m_gameID.m_nType == k_EGameIDTypeApp );
	}

	[[nodiscard]] constexpr AppId_t AppID(bool checkType = false) const noexcept
	{
		if(checkType && !IsSteamApp())
			return 0;
		return m_gameID.m_nAppID;
	}

	constexpr void SetAppID(AppId_t nAppID) noexcept
	{
		m_gameID.m_nAppID = nAppID;
	}

	[[nodiscard]] constexpr bool operator==(const CGameID& other) const noexcept {
		return m_ulGameID == other.m_ulGameID;
	}
	[[nodiscard]] constexpr auto operator<=>(const CGameID& other) const noexcept {
		return m_ulGameID <=> other.m_ulGameID;
	}

	struct GameID_t
	{
		unsigned int m_nAppID : 24;
		unsigned int m_nType : 8;
		unsigned int m_nModID : 32;
	};

	union
	{
		uint64 m_ulGameID;
		GameID_t m_gameID;
	};
};
static_assert(sizeof(CGameID) == 8, "CGameID must be 8 bytes");
static_assert(alignof(CGameID) == 8, "CGameID must be 8-byte aligned");
static_assert(std::is_standard_layout_v<CGameID>, "CGameID must be standard layout");
static_assert(std::is_trivially_copyable_v<CGameID>, "CGameID must be trivially copyable");

constexpr unsigned int k_unSteamAccountIDMask = 0xFFFFFFFF;
constexpr unsigned int k_unSteamAccountInstanceMask = 0x000FFFFF;
constexpr unsigned int k_unSteamUserDefaultInstance	= 1; // fixed instance for all individual users

struct CSteamID
{

	constexpr CSteamID() noexcept
		: m_steamid{ .m_unAll64Bits = 0 }
	{
	}

	constexpr CSteamID( uint64 ulSteamID ) noexcept
		: m_steamid{ .m_unAll64Bits = ulSteamID }
	{
	}

	//-----------------------------------------------------------------------------
	// Purpose: Sets parameters for steam ID
	// Input  : unAccountID -	32-bit account ID
	//			eUniverse -		Universe this account belongs to
	//			eAccountType -	Type of account
	//-----------------------------------------------------------------------------
	constexpr void Set( uint32 unAccountID, EUniverse eUniverse, EAccountType eAccountType ) noexcept
	{
		m_steamid.m_comp.m_unAccountID = unAccountID;
		m_steamid.m_comp.m_EUniverse = eUniverse;
		m_steamid.m_comp.m_EAccountType = eAccountType;

		if ( eAccountType == k_EAccountTypeClan || eAccountType == k_EAccountTypeGameServer )
		{
			m_steamid.m_comp.m_unAccountInstance = 0;
		}
		else
		{
			m_steamid.m_comp.m_unAccountInstance = k_unSteamUserDefaultInstance;
		}
	}

	constexpr void SetFromUint64( uint64 ulSteamID ) noexcept
	{
		m_steamid.m_unAll64Bits = ulSteamID;
	}

	[[nodiscard]] constexpr uint64 ConvertToUint64() const noexcept
	{
		return m_steamid.m_unAll64Bits;
	}

	constexpr void SetAccountID( uint32 unAccountID ) noexcept		{ m_steamid.m_comp.m_unAccountID = unAccountID; }
	[[nodiscard]] constexpr AccountID_t GetAccountID() const noexcept	{ return m_steamid.m_comp.m_unAccountID; }

	[[nodiscard]] constexpr EUniverse GetEUniverse() const noexcept		{ return m_steamid.m_comp.m_EUniverse; }
	[[nodiscard]] constexpr EAccountType GetEAccountType() const noexcept	{ return static_cast<EAccountType>(m_steamid.m_comp.m_EAccountType); }
	[[nodiscard]] constexpr unsigned int GetAccountInstance() const noexcept { return m_steamid.m_comp.m_unAccountInstance; }

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return ( m_steamid.m_comp.m_EAccountType > k_EAccountTypeInvalid && m_steamid.m_comp.m_EAccountType < k_EAccountTypeMax ) 
			&& ( m_steamid.m_comp.m_EUniverse > k_EUniverseInvalid && m_steamid.m_comp.m_EUniverse < k_EUniverseMax );
	}

	[[nodiscard]] constexpr bool operator==(const CSteamID& other) const noexcept {
		return m_steamid.m_unAll64Bits == other.m_steamid.m_unAll64Bits;
	}
	[[nodiscard]] constexpr auto operator<=>(const CSteamID& other) const noexcept {
		return m_steamid.m_unAll64Bits <=> other.m_steamid.m_unAll64Bits;
	}

	friend std::ostream& operator<<(std::ostream& os, const CSteamID& steamId){
		return os << steamId.ConvertToUint64();
	}

private:
	union
	{
		struct SteamIDComponent_t
		{
			uint32				m_unAccountID : 32;			// unique account identifier
			unsigned int		m_unAccountInstance : 20;	// dynamic instance ID
			unsigned int		m_EAccountType : 4;			// type of account - can't show as EAccountType, due to signed / unsigned difference
			EUniverse			m_EUniverse : 8;	// universe this account belongs to
		} m_comp;
		uint64 m_unAll64Bits;
	} m_steamid;

};
static_assert(sizeof(CSteamID) == 8, "CSteamID must be 8 bytes");
static_assert(alignof(CSteamID) == 8, "CSteamID must be 8-byte aligned");
static_assert(std::is_standard_layout_v<CSteamID>, "CSteamID must be standard layout");
static_assert(std::is_trivially_copyable_v<CSteamID>, "CSteamID must be trivially copyable");

template <>
struct std::formatter<CSteamID> {
	constexpr auto parse(std::format_parse_context& ctx) {
		return ctx.begin();
	}
	auto format(const CSteamID& id, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "{}", id.ConvertToUint64());
	}
};

struct CAppData
{
	void** vfptr;
	AppId_t nAppID;
	uint32 ChangeNumber;
	uint32 LastChangeTimeStamp;
	bool bSkipFlag;
	bool bDeniedToken;
	bool bMissingToken;
	uint64 accessToken;
	uint8 sha1Hash[20];

	bool HasEmptyAppInfoSha() const {
		static constexpr uint8 kEmptySha1[sizeof(sha1Hash)] = {};
		return memcmp(sha1Hash, kEmptySha1, sizeof(sha1Hash)) == 0;
	}

	bool IsUnresolvedAppInfo() const {
		return HasEmptyAppInfoSha() && !bSkipFlag;
	}
};
static_assert(sizeof(CAppData) == 56, "CAppData must be 56 bytes");
static_assert(alignof(CAppData) == 8, "CAppData must be 8-byte aligned");
static_assert(offsetof(CAppData, nAppID) == 0x08, "nAppID offset mismatch");
static_assert(offsetof(CAppData, ChangeNumber) == 0x0C, "ChangeNumber offset mismatch");
static_assert(offsetof(CAppData, LastChangeTimeStamp) == 0x10, "LastChangeTimeStamp offset mismatch");
static_assert(offsetof(CAppData, accessToken) == 0x18, "accessToken offset mismatch");
static_assert(offsetof(CAppData, sha1Hash) == 0x20, "sha1Hash offset mismatch");
static_assert(std::is_standard_layout_v<CAppData>, "CAppData must be standard layout");

#pragma pack(push,1)
struct CSteamApp
{
	void** vfptr;
	CGameID GameID;
	AppId_t nAppID;
	uint16 _unknown1;
	uint16 _unknown2;
	EAppReleaseState ReleaseState;
	EAppOwnershipFlags OwnershipFlags;
	EAppState AppStateFlags;
	CSteamID SteamID;
	uint32 PurchasedTime;
	uint32 ChangeNumber;
	uint32 LicenseExpirationTime;
	AppId_t MasterSubAppID;
	uint32 eProtoAppType;
	AppId_t ParentAppID;
};
#pragma pack(pop)
static_assert(sizeof(CSteamApp) == (sizeof(void*) == 8 ? 0x44 : 0x40), "CSteamApp size mismatch");
static_assert(alignof(CSteamApp) == 1, "CSteamApp must be 1-byte aligned");
static_assert(offsetof(CSteamApp, GameID) == 0x08, "GameID offset mismatch");
static_assert(offsetof(CSteamApp, nAppID) == 0x10, "nAppID offset mismatch");
static_assert(offsetof(CSteamApp, ReleaseState) == 0x18, "ReleaseState offset mismatch");
static_assert(offsetof(CSteamApp, SteamID) == 0x24, "SteamID offset mismatch");
static_assert(offsetof(CSteamApp, MasterSubAppID) == 0x38, "MasterSubAppID offset mismatch");
static_assert(std::is_standard_layout_v<CSteamApp>, "CSteamApp must be standard layout");
