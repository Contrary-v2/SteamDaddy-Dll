#pragma once

// Primitive types and ID handles used throughout steamclient internals.

#include <cstdint>

using byte = unsigned char;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

// Domain-specific ID types
using AppId_t = uint32;
using DepotId_t = uint32;
using PackageId_t = uint32;
using PID_t = uint32;
using AccountID_t = uint32;
using HAuthTicket = uint32;
using HCONNECTION = uint32;

using HSteamPipe = int32;
using HSteamUser = int32;

using GID_t = uint64;
using SteamAPICall_t = uint64;
using JobID_t = GID_t;
using ManifestId_t = uint64;

// Sentinel invalid / special constants
constexpr AppId_t k_uAppIdInvalid = 0x0;
constexpr DepotId_t k_uDepotIdInvalid = 0x0;
constexpr AccountID_t k_uAccountIDInvalid = 0x0;
constexpr PackageId_t k_uPackageIdFreeSub = 0x0;
constexpr PackageId_t k_uPackageIdInvalid = 0xFFFF'FFFFu;
constexpr PackageId_t k_uPackageIdWallet = 0xFFFF'FFFEu;
constexpr PackageId_t k_uPackageIdMicroTxn = 0xFFFF'FFFDu;
constexpr GID_t k_GIDNil = 0xFFFF'FFFF'FFFF'FFFFull;
constexpr SteamAPICall_t k_uAPICallInvalid = 0x0;
constexpr ManifestId_t k_uManifestIdInvalid = 0x0;

constexpr HSteamPipe k_HSteamPipeInvalid = 0;
constexpr HSteamUser k_HSteamUserInvalid = 0;
constexpr HAuthTicket k_HAuthTicketInvalid = 0;