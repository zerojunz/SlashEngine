#pragma once

// A stand-in for the EOS SDK's headers, so the eos package's glue code compiles and runs in the
// tests without Epic's SDK, which comes from the Epic Developer Portal. It declares only what the
// glue calls, with the names and shapes Epic's public documentation gives, and plays Epic Online
// Services from fake_eos::state: the tests set the player, achievements, stats and leaderboards
// there, and read back what the glue did. eos_auth.h, eos_connect.h and the others include this.

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define EOS_CALL
#define EOS_TRUE 1
#define EOS_FALSE 0
#define EOS_PRODUCTUSERID_MAX_LENGTH 32
#define EOS_ACHIEVEMENTS_ACHIEVEMENT_UNLOCKTIME_UNDEFINED -1

#define EOS_INITIALIZE_API_LATEST 4
#define EOS_PLATFORM_OPTIONS_API_LATEST 14
#define EOS_AUTH_CREDENTIALS_API_LATEST 4
#define EOS_AUTH_LOGIN_API_LATEST 3
#define EOS_AUTH_COPYIDTOKEN_API_LATEST 1
#define EOS_CONNECT_CREDENTIALS_API_LATEST 1
#define EOS_CONNECT_LOGIN_API_LATEST 2
#define EOS_CONNECT_CREATEUSER_API_LATEST 1
#define EOS_CONNECT_COPYPRODUCTUSERINFO_API_LATEST 1
#define EOS_ACHIEVEMENTS_QUERYDEFINITIONS_API_LATEST 3
#define EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST 2
#define EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYACHIEVEMENTID_API_LATEST 2
#define EOS_ACHIEVEMENTS_UNLOCKACHIEVEMENTS_API_LATEST 1
#define EOS_STATS_INGESTDATA_API_LATEST 1
#define EOS_STATS_INGESTSTAT_API_LATEST 3
#define EOS_LEADERBOARDS_QUERYLEADERBOARDRANKS_API_LATEST 2
#define EOS_LEADERBOARDS_GETLEADERBOARDRECORDCOUNT_API_LATEST 1
#define EOS_LEADERBOARDS_COPYLEADERBOARDRECORDBYINDEX_API_LATEST 2
#define EOS_LEADERBOARDS_COPYLEADERBOARDRECORDBYUSERID_API_LATEST 2

using EOS_Bool = std::int32_t;

enum EOS_EResult : std::int32_t {
    EOS_Success = 0,
    EOS_NoConnection = 1,
    EOS_InvalidCredentials = 2,
    EOS_InvalidUser = 3,
    EOS_InvalidAuth = 4,
    EOS_NotFound = 13,
    EOS_AlreadyConfigured = 16,
    EOS_OperationWillRetry = 23,
    EOS_Canceled = 28,
};

inline const char* EOS_EResult_ToString(EOS_EResult result) {
    switch (result) {
    case EOS_Success:
        return "EOS_Success";
    case EOS_InvalidUser:
        return "EOS_InvalidUser";
    case EOS_InvalidAuth:
        return "EOS_InvalidAuth";
    case EOS_NotFound:
        return "EOS_NotFound";
    case EOS_Canceled:
        return "EOS_Canceled";
    default:
        return "EOS_UnexpectedError";
    }
}

inline EOS_Bool EOS_EResult_IsOperationComplete(EOS_EResult result) {
    return result == EOS_OperationWillRetry ? EOS_FALSE : EOS_TRUE;
}

struct EOS_PlatformHandle {};
struct EOS_AuthHandle {};
struct EOS_ConnectHandle {};
struct EOS_AchievementsHandle {};
struct EOS_StatsHandle {};
struct EOS_LeaderboardsHandle {};
struct EOS_EpicAccountIdDetails {
    std::string id;
};
struct EOS_ProductUserIdDetails {
    std::string id;
};
struct EOS_ContinuanceTokenDetails {};
using EOS_HPlatform = EOS_PlatformHandle*;
using EOS_HAuth = EOS_AuthHandle*;
using EOS_HConnect = EOS_ConnectHandle*;
using EOS_HAchievements = EOS_AchievementsHandle*;
using EOS_HStats = EOS_StatsHandle*;
using EOS_HLeaderboards = EOS_LeaderboardsHandle*;
using EOS_EpicAccountId = EOS_EpicAccountIdDetails*;
using EOS_ProductUserId = EOS_ProductUserIdDetails*;
using EOS_ContinuanceToken = EOS_ContinuanceTokenDetails*;

enum EOS_ELoginCredentialType : std::int32_t {
    EOS_LCT_Password = 0,
    EOS_LCT_ExchangeCode = 1,
    EOS_LCT_PersistentAuth = 2,
    EOS_LCT_DeviceCode = 3,
    EOS_LCT_Developer = 4,
    EOS_LCT_RefreshToken = 5,
    EOS_LCT_AccountPortal = 6,
    EOS_LCT_ExternalAuth = 7,
};
enum EOS_EAuthScopeFlags : std::int32_t { EOS_AS_NoFlags = 0, EOS_AS_BasicProfile = 1 };
enum EOS_EExternalCredentialType : std::int32_t { EOS_ECT_EPIC = 0, EOS_ECT_EPIC_ID_TOKEN = 16 };

struct EOS_InitializeOptions {
    std::int32_t ApiVersion;
    void* AllocateMemoryFunction;
    void* ReallocateMemoryFunction;
    void* ReleaseMemoryFunction;
    const char* ProductName;
    const char* ProductVersion;
    void* Reserved;
    void* SystemInitializeOptions;
    void* OverrideThreadAffinity;
};
struct EOS_Platform_ClientCredentials {
    const char* ClientId;
    const char* ClientSecret;
};
struct EOS_Platform_Options {
    std::int32_t ApiVersion;
    void* Reserved;
    const char* ProductId;
    const char* SandboxId;
    EOS_Platform_ClientCredentials ClientCredentials;
    EOS_Bool bIsServer;
    const char* EncryptionKey;
    const char* OverrideCountryCode;
    const char* OverrideLocaleCode;
    const char* DeploymentId;
    std::uint64_t Flags;
    const char* CacheDirectory;
    std::uint32_t TickBudgetInMilliseconds;
};

struct EOS_Auth_Credentials {
    std::int32_t ApiVersion;
    const char* Id;
    const char* Token;
    EOS_ELoginCredentialType Type;
    void* SystemAuthCredentialsOptions;
    EOS_EExternalCredentialType ExternalType;
};
struct EOS_Auth_LoginOptions {
    std::int32_t ApiVersion;
    const EOS_Auth_Credentials* Credentials;
    EOS_EAuthScopeFlags ScopeFlags;
    std::uint64_t LoginFlags;
};
struct EOS_Auth_LoginCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_EpicAccountId LocalUserId;
    const void* PinGrantInfo;
    EOS_ContinuanceToken ContinuanceToken;
    const void* AccountFeatureRestrictedInfo_DEPRECATED;
    EOS_EpicAccountId SelectedAccountId;
};
struct EOS_Auth_CopyIdTokenOptions {
    std::int32_t ApiVersion;
    EOS_EpicAccountId AccountId;
};
struct EOS_Auth_IdToken {
    std::int32_t ApiVersion;
    EOS_EpicAccountId AccountId;
    const char* JsonWebToken;
};

struct EOS_Connect_Credentials {
    std::int32_t ApiVersion;
    const char* Token;
    EOS_EExternalCredentialType Type;
};
struct EOS_Connect_LoginOptions {
    std::int32_t ApiVersion;
    const EOS_Connect_Credentials* Credentials;
    const void* UserLoginInfo;
};
struct EOS_Connect_LoginCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ContinuanceToken ContinuanceToken;
};
struct EOS_Connect_CreateUserOptions {
    std::int32_t ApiVersion;
    EOS_ContinuanceToken ContinuanceToken;
};
struct EOS_Connect_CreateUserCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId LocalUserId;
};
struct EOS_Connect_CopyProductUserInfoOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
};
struct EOS_Connect_ExternalAccountInfo {
    std::int32_t ApiVersion;
    EOS_ProductUserId ProductUserId;
    const char* DisplayName;
    const char* AccountId;
    EOS_EExternalCredentialType AccountIdType;
    std::int64_t LastLoginTime;
};

struct EOS_Achievements_QueryDefinitionsOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    EOS_EpicAccountId EpicUserId_DEPRECATED;
    const char** HiddenAchievementIds_DEPRECATED;
    std::uint32_t HiddenAchievementsCount_DEPRECATED;
};
struct EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
};
struct EOS_Achievements_QueryPlayerAchievementsOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    EOS_ProductUserId LocalUserId;
};
struct EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId TargetUserId;
    EOS_ProductUserId LocalUserId;
};
struct EOS_Achievements_CopyPlayerAchievementByAchievementIdOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    const char* AchievementId;
    EOS_ProductUserId LocalUserId;
};
struct EOS_Achievements_PlayerAchievement {
    std::int32_t ApiVersion;
    const char* AchievementId;
    double Progress;
    std::int64_t UnlockTime;
    std::int32_t StatInfoCount;
    const void* StatInfo;
    const char* DisplayName;
    const char* Description;
    const char* IconURL;
    const char* FlavorText;
};
struct EOS_Achievements_UnlockAchievementsOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId UserId;
    const char** AchievementIds;
    std::uint32_t AchievementsCount;
};
struct EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId UserId;
    std::uint32_t AchievementsCount;
};

struct EOS_Stats_IngestData {
    std::int32_t ApiVersion;
    const char* StatName;
    std::int32_t IngestAmount;
};
struct EOS_Stats_IngestStatOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const EOS_Stats_IngestData* Stats;
    std::uint32_t StatsCount;
    EOS_ProductUserId TargetUserId;
};
struct EOS_Stats_IngestStatCompleteCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId TargetUserId;
};

struct EOS_Leaderboards_QueryLeaderboardRanksOptions {
    std::int32_t ApiVersion;
    const char* LeaderboardId;
    EOS_ProductUserId LocalUserId;
};
struct EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    const char* LeaderboardId;
};
struct EOS_Leaderboards_GetLeaderboardRecordCountOptions {
    std::int32_t ApiVersion;
};
struct EOS_Leaderboards_CopyLeaderboardRecordByIndexOptions {
    std::int32_t ApiVersion;
    std::uint32_t LeaderboardRecordIndex;
};
struct EOS_Leaderboards_CopyLeaderboardRecordByUserIdOptions {
    std::int32_t ApiVersion;
    EOS_ProductUserId UserId;
};
struct EOS_Leaderboards_LeaderboardRecord {
    std::int32_t ApiVersion;
    EOS_ProductUserId UserId;
    std::uint32_t Rank;
    std::int32_t Score;
    const char* UserDisplayName;
};

namespace fake_eos {

struct Achievement {
    double progress = 0;
    bool unlocked = false;
};

struct Record {
    std::string user; // product user id
    std::string name;
    std::int32_t score = 0;
};

// What the fake EOS holds. The tests set the services up, then read back what the glue did.
struct State {
    // Set up by the tests.
    bool configured = true;    // EOS_Platform_Create works (the package's keys were set)
    bool kept_login = true;    // a persistent login from an earlier sign-in
    bool portal_works = true;  // the player signs in on Epic's account portal
    bool new_player = false;   // first time in the game: Connect needs a new product user
    std::string exchange_code; // the launcher's code EOS accepts, if any
    std::string player = "0002a4f1c8e7b5d3"; // the product user id
    std::string name = "Player";
    std::map<std::string, Achievement> achievements;
    std::map<std::string, std::int32_t> stats;               // the stats the game has, by name
    std::map<std::string, std::vector<Record>> leaderboards; // ranked best first

    // Seen by the tests.
    bool initialized = false;
    bool platform = false;
    std::string product_id;
    std::vector<std::string> logins;   // each Auth login's type, and the Connect login
    std::vector<std::string> ingested; // "stat amount"

    // Inside the fake.
    std::deque<std::function<void()>> queue; // callbacks the next tick runs
    std::map<std::string, std::unique_ptr<EOS_ProductUserIdDetails>> users;
    EOS_EpicAccountIdDetails account{"epic-account"};
    EOS_ContinuanceTokenDetails continuance;
    std::vector<Record> loaded; // the last leaderboard ranks query's records
    std::string name_copy;
};

inline State state;

inline void reset() {
    state = State{};
}

inline EOS_ProductUserId user(const std::string& id) {
    auto& slot = state.users[id];
    if (!slot) {
        slot = std::make_unique<EOS_ProductUserIdDetails>(EOS_ProductUserIdDetails{id});
    }
    return slot.get();
}

// Runs callback with info on the next tick, as EOS does.
template <typename Info, typename Callback>
void later(Info info, void* client, Callback callback) {
    info.ClientData = client;
    state.queue.emplace_back([info, callback] { callback(&info); });
}

inline EOS_PlatformHandle platform_handle;
inline EOS_AuthHandle auth_handle;
inline EOS_ConnectHandle connect_handle;
inline EOS_AchievementsHandle achievements_handle;
inline EOS_StatsHandle stats_handle;
inline EOS_LeaderboardsHandle leaderboards_handle;

} // namespace fake_eos

inline EOS_EResult EOS_Initialize(const EOS_InitializeOptions* options) {
    if (options->ProductName == nullptr || options->ProductName[0] == '\0') {
        return EOS_InvalidCredentials;
    }
    fake_eos::state.initialized = true;
    return EOS_Success;
}

inline EOS_EResult EOS_Shutdown() {
    fake_eos::state.initialized = false;
    return EOS_Success;
}

inline EOS_HPlatform EOS_Platform_Create(const EOS_Platform_Options* options) {
    if (!fake_eos::state.configured || options->ProductId == nullptr) {
        return nullptr;
    }
    fake_eos::state.platform = true;
    fake_eos::state.product_id = options->ProductId;
    return &fake_eos::platform_handle;
}

inline void EOS_Platform_Release(EOS_HPlatform /*platform*/) {
    fake_eos::state.platform = false;
}

inline void EOS_Platform_Tick(EOS_HPlatform /*platform*/) {
    auto queue = std::move(fake_eos::state.queue);
    fake_eos::state.queue.clear();
    for (auto& callback : queue) {
        callback();
    }
}

inline EOS_HAuth EOS_Platform_GetAuthInterface(EOS_HPlatform /*platform*/) {
    return &fake_eos::auth_handle;
}
inline EOS_HConnect EOS_Platform_GetConnectInterface(EOS_HPlatform /*platform*/) {
    return &fake_eos::connect_handle;
}
inline EOS_HAchievements EOS_Platform_GetAchievementsInterface(EOS_HPlatform /*platform*/) {
    return &fake_eos::achievements_handle;
}
inline EOS_HStats EOS_Platform_GetStatsInterface(EOS_HPlatform /*platform*/) {
    return &fake_eos::stats_handle;
}
inline EOS_HLeaderboards EOS_Platform_GetLeaderboardsInterface(EOS_HPlatform /*platform*/) {
    return &fake_eos::leaderboards_handle;
}

inline EOS_EResult EOS_ProductUserId_ToString(EOS_ProductUserId id, char* out, std::int32_t* size) {
    if (id == nullptr || static_cast<std::int32_t>(id->id.size()) + 1 > *size) {
        return EOS_InvalidUser;
    }
    std::memcpy(out, id->id.c_str(), id->id.size() + 1);
    *size = static_cast<std::int32_t>(id->id.size()) + 1;
    return EOS_Success;
}

template <typename Callback>
void EOS_Auth_Login(EOS_HAuth /*auth*/, const EOS_Auth_LoginOptions* options, void* client,
                    Callback callback) {
    auto& state = fake_eos::state;
    const EOS_Auth_Credentials& credentials = *options->Credentials;
    EOS_Auth_LoginCallbackInfo info{};
    info.ResultCode = EOS_InvalidAuth;
    if (credentials.Type == EOS_LCT_ExchangeCode) {
        state.logins.push_back(std::string("ExchangeCode ") + credentials.Token);
        if (!state.exchange_code.empty() && state.exchange_code == credentials.Token) {
            info.ResultCode = EOS_Success;
        }
    } else if (credentials.Type == EOS_LCT_PersistentAuth) {
        state.logins.emplace_back("PersistentAuth");
        info.ResultCode = state.kept_login ? EOS_Success : EOS_InvalidAuth;
    } else if (credentials.Type == EOS_LCT_AccountPortal) {
        state.logins.emplace_back("AccountPortal");
        info.ResultCode = state.portal_works ? EOS_Success : EOS_Canceled;
    }
    if (info.ResultCode == EOS_Success) {
        info.LocalUserId = &state.account;
    }
    fake_eos::later(info, client, callback);
}

inline EOS_EResult EOS_Auth_CopyIdToken(EOS_HAuth /*auth*/,
                                        const EOS_Auth_CopyIdTokenOptions* options,
                                        EOS_Auth_IdToken** token) {
    *token = new EOS_Auth_IdToken{1, options->AccountId, "id-token"};
    return EOS_Success;
}

inline void EOS_Auth_IdToken_Release(EOS_Auth_IdToken* token) {
    delete token;
}

template <typename Callback>
void EOS_Connect_Login(EOS_HConnect /*connect*/, const EOS_Connect_LoginOptions* options,
                       void* client, Callback callback) {
    auto& state = fake_eos::state;
    state.logins.push_back(std::string("Connect ") + options->Credentials->Token);
    EOS_Connect_LoginCallbackInfo info{};
    if (options->Credentials->Type != EOS_ECT_EPIC_ID_TOKEN) {
        info.ResultCode = EOS_InvalidAuth;
    } else if (state.new_player) {
        info.ResultCode = EOS_InvalidUser;
        info.ContinuanceToken = &state.continuance;
    } else {
        info.ResultCode = EOS_Success;
        info.LocalUserId = fake_eos::user(state.player);
    }
    fake_eos::later(info, client, callback);
}

template <typename Callback>
void EOS_Connect_CreateUser(EOS_HConnect /*connect*/,
                            const EOS_Connect_CreateUserOptions* /*options*/, void* client,
                            Callback callback) {
    auto& state = fake_eos::state;
    state.logins.emplace_back("CreateUser");
    state.new_player = false;
    EOS_Connect_CreateUserCallbackInfo info{};
    info.ResultCode = EOS_Success;
    info.LocalUserId = fake_eos::user(state.player);
    fake_eos::later(info, client, callback);
}

inline EOS_EResult
EOS_Connect_CopyProductUserInfo(EOS_HConnect /*connect*/,
                                const EOS_Connect_CopyProductUserInfoOptions* options,
                                EOS_Connect_ExternalAccountInfo** info) {
    fake_eos::state.name_copy = fake_eos::state.name;
    *info = new EOS_Connect_ExternalAccountInfo{
        1, options->TargetUserId, fake_eos::state.name_copy.c_str(), "epic-account", EOS_ECT_EPIC,
        0};
    return EOS_Success;
}

inline void EOS_Connect_ExternalAccountInfo_Release(EOS_Connect_ExternalAccountInfo* info) {
    delete info;
}

template <typename Callback>
void EOS_Achievements_QueryDefinitions(EOS_HAchievements /*achievements*/,
                                       const EOS_Achievements_QueryDefinitionsOptions* /*options*/,
                                       void* client, Callback callback) {
    EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo info{};
    info.ResultCode = EOS_Success;
    fake_eos::later(info, client, callback);
}

template <typename Callback>
void EOS_Achievements_QueryPlayerAchievements(
    EOS_HAchievements /*achievements*/,
    const EOS_Achievements_QueryPlayerAchievementsOptions* options, void* client,
    Callback callback) {
    EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo info{};
    info.ResultCode = EOS_Success;
    info.TargetUserId = options->TargetUserId;
    info.LocalUserId = options->LocalUserId;
    fake_eos::later(info, client, callback);
}

inline EOS_EResult EOS_Achievements_CopyPlayerAchievementByAchievementId(
    EOS_HAchievements /*achievements*/,
    const EOS_Achievements_CopyPlayerAchievementByAchievementIdOptions* options,
    EOS_Achievements_PlayerAchievement** out) {
    const auto it = fake_eos::state.achievements.find(options->AchievementId);
    if (it == fake_eos::state.achievements.end()) {
        return EOS_NotFound;
    }
    *out = new EOS_Achievements_PlayerAchievement{};
    (*out)->AchievementId = it->first.c_str();
    (*out)->Progress = it->second.unlocked ? 1.0 : it->second.progress;
    (*out)->UnlockTime =
        it->second.unlocked ? 1764000000 : EOS_ACHIEVEMENTS_ACHIEVEMENT_UNLOCKTIME_UNDEFINED;
    return EOS_Success;
}

inline void
EOS_Achievements_PlayerAchievement_Release(EOS_Achievements_PlayerAchievement* achievement) {
    delete achievement;
}

template <typename Callback>
void EOS_Achievements_UnlockAchievements(EOS_HAchievements /*achievements*/,
                                         const EOS_Achievements_UnlockAchievementsOptions* options,
                                         void* client, Callback callback) {
    EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo info{};
    info.ResultCode = EOS_Success;
    info.UserId = options->UserId;
    for (std::uint32_t i = 0; i < options->AchievementsCount; ++i) {
        const auto it = fake_eos::state.achievements.find(options->AchievementIds[i]);
        if (it == fake_eos::state.achievements.end()) {
            info.ResultCode = EOS_NotFound;
        } else {
            it->second.unlocked = true;
            ++info.AchievementsCount;
        }
    }
    fake_eos::later(info, client, callback);
}

template <typename Callback>
void EOS_Stats_IngestStat(EOS_HStats /*stats*/, const EOS_Stats_IngestStatOptions* options,
                          void* client, Callback callback) {
    EOS_Stats_IngestStatCompleteCallbackInfo info{};
    info.ResultCode = EOS_Success;
    for (std::uint32_t i = 0; i < options->StatsCount; ++i) {
        const EOS_Stats_IngestData& data = options->Stats[i];
        const auto it = fake_eos::state.stats.find(data.StatName);
        if (it == fake_eos::state.stats.end()) {
            info.ResultCode = EOS_NotFound;
        } else {
            it->second = data.IngestAmount;
            fake_eos::state.ingested.push_back(std::string(data.StatName) + " " +
                                               std::to_string(data.IngestAmount));
        }
    }
    fake_eos::later(info, client, callback);
}

template <typename Callback>
void EOS_Leaderboards_QueryLeaderboardRanks(
    EOS_HLeaderboards /*leaderboards*/,
    const EOS_Leaderboards_QueryLeaderboardRanksOptions* options, void* client, Callback callback) {
    auto& state = fake_eos::state;
    EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo info{};
    const auto it = state.leaderboards.find(options->LeaderboardId);
    info.ResultCode = it == state.leaderboards.end() ? EOS_NotFound : EOS_Success;
    std::vector<fake_eos::Record> records =
        it == state.leaderboards.end() ? std::vector<fake_eos::Record>() : it->second;
    state.queue.emplace_back([info, client, callback, records] {
        fake_eos::state.loaded = records;
        auto done = info;
        done.ClientData = client;
        callback(&done);
    });
}

inline std::uint32_t EOS_Leaderboards_GetLeaderboardRecordCount(
    EOS_HLeaderboards /*leaderboards*/, const EOS_Leaderboards_GetLeaderboardRecordCountOptions*
    /*options*/) {
    return static_cast<std::uint32_t>(fake_eos::state.loaded.size());
}

inline EOS_Leaderboards_LeaderboardRecord* fake_record(std::size_t index) {
    const fake_eos::Record& record = fake_eos::state.loaded[index];
    return new EOS_Leaderboards_LeaderboardRecord{1, fake_eos::user(record.user),
                                                  static_cast<std::uint32_t>(index + 1),
                                                  record.score, record.name.c_str()};
}

inline EOS_EResult EOS_Leaderboards_CopyLeaderboardRecordByIndex(
    EOS_HLeaderboards /*leaderboards*/,
    const EOS_Leaderboards_CopyLeaderboardRecordByIndexOptions* options,
    EOS_Leaderboards_LeaderboardRecord** out) {
    if (options->LeaderboardRecordIndex >= fake_eos::state.loaded.size()) {
        return EOS_NotFound;
    }
    *out = fake_record(options->LeaderboardRecordIndex);
    return EOS_Success;
}

inline EOS_EResult EOS_Leaderboards_CopyLeaderboardRecordByUserId(
    EOS_HLeaderboards /*leaderboards*/,
    const EOS_Leaderboards_CopyLeaderboardRecordByUserIdOptions* options,
    EOS_Leaderboards_LeaderboardRecord** out) {
    for (std::size_t i = 0; i < fake_eos::state.loaded.size(); ++i) {
        if (options->UserId != nullptr && fake_eos::state.loaded[i].user == options->UserId->id) {
            *out = fake_record(i);
            return EOS_Success;
        }
    }
    return EOS_NotFound;
}

inline void EOS_Leaderboards_LeaderboardRecord_Release(EOS_Leaderboards_LeaderboardRecord* record) {
    delete record;
}
