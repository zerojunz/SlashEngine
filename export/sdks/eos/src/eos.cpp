// The eos package's games backend: the Epic player, achievements and leaderboards through Epic
// Online Services, from the developer's own copy of the EOS SDK. Calls finish in their
// callbacks, which EOS_Platform_Tick runs each tick on the game's thread.

#include "slash/services/services.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <eos_achievements.h>
#include <eos_auth.h>
#include <eos_connect.h>
#include <eos_leaderboards.h>
#include <eos_sdk.h>
#include <eos_stats.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: shellapi.h needs its types.
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#endif

// The game's ids, from the product's pages in the Epic Developer Portal, given to the export in
// the environment (see the package's keys).
#ifndef SLASH_EOS_PRODUCT_ID
#define SLASH_EOS_PRODUCT_ID ""
#endif
#ifndef SLASH_EOS_SANDBOX_ID
#define SLASH_EOS_SANDBOX_ID ""
#endif
#ifndef SLASH_EOS_DEPLOYMENT_ID
#define SLASH_EOS_DEPLOYMENT_ID ""
#endif
#ifndef SLASH_EOS_CLIENT_ID
#define SLASH_EOS_CLIENT_ID ""
#endif
#ifndef SLASH_EOS_CLIENT_SECRET
#define SLASH_EOS_CLIENT_SECRET ""
#endif
#ifndef SLASH_EOS_PRODUCT_NAME
#define SLASH_EOS_PRODUCT_NAME ""
#endif

namespace slash_eos {

// The exchange code the Epic Games Launcher passes a game it starts ("-AUTH_PASSWORD=<code>" with
// "-AUTH_TYPE=exchangecode"), or "" when the game wasn't started by the launcher.
std::string exchange_code(std::span<const std::string> args) {
    constexpr std::string_view kPassword = "-AUTH_PASSWORD=";
    constexpr std::string_view kType = "-AUTH_TYPE=";
    std::string code;
    std::string type;
    for (const std::string& arg : args) {
        if (arg.starts_with(kPassword)) {
            code = arg.substr(kPassword.size());
        } else if (arg.starts_with(kType)) {
            type = arg.substr(kType.size());
        }
    }
    std::ranges::transform(type, type.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return type == "exchangecode" ? code : std::string();
}

namespace {

namespace services = slash::services;

// This process's arguments, as the launcher gave them.
std::vector<std::string> program_arguments() {
    std::vector<std::string> args;
#if defined(_WIN32)
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; wide != nullptr && i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>(std::max(size, 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
        arg.resize(arg.empty() ? 0 : arg.size() - 1);
        args.push_back(std::move(arg));
    }
    LocalFree(wide);
#elif defined(__APPLE__)
    char** argv = *_NSGetArgv();
    for (int i = 0; i < *_NSGetArgc(); ++i) {
        args.emplace_back(argv[i]);
    }
#else
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    for (std::string arg; std::getline(in, arg, '\0');) {
        args.push_back(std::move(arg));
    }
#endif
    return args;
}

constexpr std::string_view kNotRunning =
    "Epic Online Services didn't start: export the game with EOS_PRODUCT_ID, EOS_SANDBOX_ID, "
    "EOS_DEPLOYMENT_ID, EOS_CLIENT_ID and EOS_CLIENT_SECRET set, from the product's pages in the "
    "Epic Developer Portal";
constexpr std::string_view kSignedOut = "sign the player in with Epic first";

std::string result_text(EOS_EResult result) {
    return EOS_EResult_ToString(result);
}

std::string id_text(EOS_ProductUserId id) {
    char text[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
    std::int32_t size = static_cast<std::int32_t>(std::size(text));
    return EOS_ProductUserId_ToString(id, text, &size) == EOS_Success ? std::string(text)
                                                                      : std::string();
}

// A call's completion, carried through EOS as the call's ClientData.
template <typename Info>
struct Call {
    std::function<void(const Info&)> done;
};

template <typename Info>
void* call(std::function<void(const Info&)> done) {
    return new Call<Info>{std::move(done)}; // NOLINT(cppcoreguidelines-owning-memory): finish
}

// Every call's callback: runs its completion once EOS says the call is over, then frees it.
template <typename Info>
void EOS_CALL finish(const Info* info) {
    if (EOS_EResult_IsOperationComplete(info->ResultCode) == EOS_FALSE) {
        return; // EOS retries and calls back again
    }
    const std::unique_ptr<Call<Info>> pending(static_cast<Call<Info>*>(info->ClientData));
    pending->done(*info);
}

class EpicGames final : public services::GamesBackend {
public:
    EpicGames() {
        EOS_InitializeOptions init{};
        init.ApiVersion = EOS_INITIALIZE_API_LATEST;
        const std::string_view product_name = SLASH_EOS_PRODUCT_NAME;
        init.ProductName = product_name.empty() ? "Game" : SLASH_EOS_PRODUCT_NAME;
        init.ProductVersion = "1";
        const EOS_EResult initialized = EOS_Initialize(&init);
        if (initialized != EOS_Success && initialized != EOS_AlreadyConfigured) {
            return;
        }
        initialized_ = initialized == EOS_Success;
        EOS_Platform_Options options{};
        options.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        options.ProductId = SLASH_EOS_PRODUCT_ID;
        options.SandboxId = SLASH_EOS_SANDBOX_ID;
        options.DeploymentId = SLASH_EOS_DEPLOYMENT_ID;
        options.ClientCredentials.ClientId = SLASH_EOS_CLIENT_ID;
        options.ClientCredentials.ClientSecret = SLASH_EOS_CLIENT_SECRET;
        options.bIsServer = EOS_FALSE;
        platform_ = EOS_Platform_Create(&options);
    }
    ~EpicGames() override {
        if (platform_ != nullptr) {
            EOS_Platform_Release(platform_);
        }
        if (initialized_) {
            EOS_Shutdown();
        }
    }
    EpicGames(const EpicGames&) = delete;
    EpicGames& operator=(const EpicGames&) = delete;
    EpicGames(EpicGames&&) = delete;
    EpicGames& operator=(EpicGames&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "epic"; }

    void start(const std::shared_ptr<services::Reports>& reports) override { reports_ = reports; }

    void poll() override {
        if (platform_ != nullptr) {
            EOS_Platform_Tick(platform_);
        }
    }

    // The launcher's exchange code when it started the game; else the login EOS kept from the
    // last time, and when the game asks, Epic's account portal.
    void sign_in(bool interactive) override {
        if (platform_ == nullptr) {
            reports_->sign_in_failed(kNotRunning);
            return;
        }
        const std::string code = exchange_code(program_arguments());
        if (!code.empty()) {
            log_in(EOS_LCT_ExchangeCode, code, false);
        } else {
            log_in(EOS_LCT_PersistentAuth, "", interactive);
        }
    }

    // Epic's achievements are named by their ids, which are the game's names.
    void load_achievements(std::span<const services::AchievementUnit> units) override {
        if (!ready("", &services::Reports::achievement_failed)) {
            return;
        }
        std::vector<services::AchievementUnit> wanted(units.begin(), units.end());
        EOS_Achievements_QueryDefinitionsOptions definitions{};
        definitions.ApiVersion = EOS_ACHIEVEMENTS_QUERYDEFINITIONS_API_LATEST;
        definitions.LocalUserId = user_;
        EOS_Achievements_QueryDefinitions(
            achievements(), &definitions,
            call<EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo>(
                [this,
                 wanted](const EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo& info) {
                    if (info.ResultCode != EOS_Success) {
                        reports_->achievement_failed("", "Epic couldn't load the achievements: " +
                                                             result_text(info.ResultCode));
                        return;
                    }
                    query_player(wanted);
                }),
            finish<EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo>);
    }

    void unlock(const services::AchievementUnit& unit) override {
        if (!ready(unit.achievement, &services::Reports::achievement_failed)) {
            return;
        }
        const std::string achievement = unit.achievement;
        const char* ids[] = {achievement.c_str()};
        EOS_Achievements_UnlockAchievementsOptions options{};
        options.ApiVersion = EOS_ACHIEVEMENTS_UNLOCKACHIEVEMENTS_API_LATEST;
        options.UserId = user_;
        options.AchievementIds = ids;
        options.AchievementsCount = 1;
        EOS_Achievements_UnlockAchievements(
            achievements(), &options,
            call<EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo>(
                [this, achievement](
                    const EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo& info) {
                    if (info.ResultCode != EOS_Success) {
                        reports_->achievement_failed(achievement,
                                                     missing("achievement", achievement, info));
                    }
                }),
            finish<EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo>);
    }

    // Epic counts progress in a stat: the steps go to the stat named like the achievement, whose
    // threshold in the Portal unlocks it; reaching the achievement's steps unlocks it here too.
    void set_steps(const services::AchievementUnit& unit, int steps) override {
        if (!ready(unit.achievement, &services::Reports::achievement_failed)) {
            return;
        }
        const std::string achievement = unit.achievement;
        ingest(achievement, steps, [this, achievement](EOS_EResult result) {
            if (result != EOS_Success) {
                reports_->achievement_failed(
                    achievement, "Epic has no stat \"" + achievement +
                                     "\" counting the achievement's steps (" + result_text(result) +
                                     "); add one named like it in the Portal");
            }
        });
        if (steps >= unit.steps) {
            unlock(unit);
        }
    }

    // A score goes to the stat named like the leaderboard, which the leaderboard ranks.
    void submit_score(const services::LeaderboardUnit& unit, std::int64_t score) override {
        if (!ready(unit.leaderboard, &services::Reports::score_failed)) {
            return;
        }
        if (score < std::numeric_limits<std::int32_t>::min() ||
            score > std::numeric_limits<std::int32_t>::max()) {
            reports_->score_failed(unit.leaderboard, "Epic stats hold 32-bit scores");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        ingest(leaderboard, static_cast<std::int32_t>(score),
               [this, leaderboard, score](EOS_EResult result) {
                   if (result != EOS_Success) {
                       reports_->score_failed(leaderboard,
                                              "Epic has no stat \"" + leaderboard +
                                                  "\" for the leaderboard (" + result_text(result) +
                                                  "); rank the leaderboard by a stat named "
                                                  "like it in the Portal");
                   } else {
                       reports_->score_submitted(leaderboard, score);
                   }
               });
    }

    void load_scores(const services::LeaderboardUnit& unit,
                     const services::ScoreQuery& query) override {
        if (!ready(unit.leaderboard, &services::Reports::scores_failed)) {
            return;
        }
        if (query.time != services::TimeSpan::AllTime) {
            reports_->scores_failed(unit.leaderboard,
                                    "Epic leaderboards keep the scores of the time window set in "
                                    "the Portal; load all_time");
            return;
        }
        if (query.collection != services::Collection::Global) {
            reports_->scores_failed(unit.leaderboard, "Epic leaderboards load global ranks only");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        EOS_Leaderboards_QueryLeaderboardRanksOptions options{};
        options.ApiVersion = EOS_LEADERBOARDS_QUERYLEADERBOARDRANKS_API_LATEST;
        options.LeaderboardId = leaderboard.c_str();
        options.LocalUserId = user_;
        EOS_Leaderboards_QueryLeaderboardRanks(
            leaderboards(), &options,
            call<EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo>(
                [this, leaderboard, count = query.count](
                    const EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo& info) {
                    if (info.ResultCode != EOS_Success) {
                        reports_->scores_failed(leaderboard,
                                                missing("leaderboard", leaderboard, info));
                        return;
                    }
                    reports_->scores(leaderboard, read_ranks(count));
                }),
            finish<EOS_Leaderboards_OnQueryLeaderboardRanksCompleteCallbackInfo>);
    }

    // EOS has no achievements or leaderboards screens of its own: games draw theirs.
    void show_achievements() override { reports_->games_ui_closed(); }

    void show_leaderboards(const services::LeaderboardUnit* unit) override {
        (void)unit;
        reports_->games_ui_closed();
    }

private:
    [[nodiscard]] EOS_HAchievements achievements() const {
        return EOS_Platform_GetAchievementsInterface(platform_);
    }
    [[nodiscard]] EOS_HLeaderboards leaderboards() const {
        return EOS_Platform_GetLeaderboardsInterface(platform_);
    }

    // Whether calls can go to EOS; reports why not through failed, with name.
    bool ready(std::string_view name,
               void (services::Reports::*failed)(std::string_view, std::string_view)) {
        if (platform_ == nullptr) {
            ((*reports_).*failed)(name, kNotRunning);
            return false;
        }
        if (user_ == nullptr) {
            ((*reports_).*failed)(name, kSignedOut);
            return false;
        }
        return true;
    }

    template <typename Info>
    static std::string missing(std::string_view what, std::string_view name, const Info& info) {
        return "Epic has no " + std::string(what) + " with the id \"" + std::string(name) + "\" (" +
               result_text(info.ResultCode) + "); give it the game's name in the Portal";
    }

    // Logs the player in to their Epic account, then to the game's services with that account.
    void log_in(EOS_ELoginCredentialType type, const std::string& token, bool portal_next) {
        EOS_Auth_Credentials credentials{};
        credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
        credentials.Type = type;
        credentials.Token = token.empty() ? nullptr : token.c_str();
        EOS_Auth_LoginOptions options{};
        options.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
        options.Credentials = &credentials;
        options.ScopeFlags = EOS_AS_BasicProfile;
        EOS_Auth_Login(EOS_Platform_GetAuthInterface(platform_), &options,
                       call<EOS_Auth_LoginCallbackInfo>(
                           [this, portal_next](const EOS_Auth_LoginCallbackInfo& info) {
                               if (info.ResultCode == EOS_Success) {
                                   connect(info.LocalUserId);
                               } else if (portal_next) {
                                   log_in(EOS_LCT_AccountPortal, "", false);
                               } else {
                                   reports_->sign_in_failed("Epic couldn't sign the player in: " +
                                                            result_text(info.ResultCode));
                               }
                           }),
                       finish<EOS_Auth_LoginCallbackInfo>);
    }

    // Logs the game's services in with the Epic account's ID token, making the player's
    // product user the first time.
    void connect(EOS_EpicAccountId account) {
        EOS_Auth_CopyIdTokenOptions copy{};
        copy.ApiVersion = EOS_AUTH_COPYIDTOKEN_API_LATEST;
        copy.AccountId = account;
        EOS_Auth_IdToken* token = nullptr;
        if (EOS_Auth_CopyIdToken(EOS_Platform_GetAuthInterface(platform_), &copy, &token) !=
            EOS_Success) {
            reports_->sign_in_failed("Epic gave no ID token for the player");
            return;
        }
        EOS_Connect_Credentials credentials{};
        credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
        credentials.Type = EOS_ECT_EPIC_ID_TOKEN;
        credentials.Token = token->JsonWebToken;
        EOS_Connect_LoginOptions options{};
        options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
        options.Credentials = &credentials;
        EOS_Connect_Login(
            EOS_Platform_GetConnectInterface(platform_), &options,
            call<EOS_Connect_LoginCallbackInfo>([this](const EOS_Connect_LoginCallbackInfo& info) {
                if (info.ResultCode == EOS_Success) {
                    signed_in(info.LocalUserId);
                } else if (info.ResultCode == EOS_InvalidUser) {
                    create_user(info.ContinuanceToken);
                } else {
                    reports_->sign_in_failed("Epic couldn't sign the player in to the game: " +
                                             result_text(info.ResultCode));
                }
            }),
            finish<EOS_Connect_LoginCallbackInfo>);
        EOS_Auth_IdToken_Release(token);
    }

    void create_user(EOS_ContinuanceToken continuance) {
        EOS_Connect_CreateUserOptions options{};
        options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
        options.ContinuanceToken = continuance;
        EOS_Connect_CreateUser(EOS_Platform_GetConnectInterface(platform_), &options,
                               call<EOS_Connect_CreateUserCallbackInfo>(
                                   [this](const EOS_Connect_CreateUserCallbackInfo& info) {
                                       if (info.ResultCode == EOS_Success) {
                                           signed_in(info.LocalUserId);
                                       } else {
                                           reports_->sign_in_failed(
                                               "Epic couldn't make the player's game account: " +
                                               result_text(info.ResultCode));
                                       }
                                   }),
                               finish<EOS_Connect_CreateUserCallbackInfo>);
    }

    void signed_in(EOS_ProductUserId user) {
        user_ = user;
        services::Player player;
        player.id = id_text(user);
        EOS_Connect_CopyProductUserInfoOptions options{};
        options.ApiVersion = EOS_CONNECT_COPYPRODUCTUSERINFO_API_LATEST;
        options.TargetUserId = user;
        EOS_Connect_ExternalAccountInfo* info = nullptr;
        if (EOS_Connect_CopyProductUserInfo(EOS_Platform_GetConnectInterface(platform_), &options,
                                            &info) == EOS_Success) {
            player.name = info->DisplayName == nullptr ? "" : info->DisplayName;
            EOS_Connect_ExternalAccountInfo_Release(info);
        }
        reports_->signed_in(std::move(player));
    }

    // The player's progress with each wanted achievement, once EOS has the definitions.
    void query_player(std::vector<services::AchievementUnit> wanted) {
        EOS_Achievements_QueryPlayerAchievementsOptions options{};
        options.ApiVersion = EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST;
        options.TargetUserId = user_;
        options.LocalUserId = user_;
        EOS_Achievements_QueryPlayerAchievements(
            achievements(), &options,
            call<EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo>(
                [this, wanted = std::move(wanted)](
                    const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo& info) {
                    if (info.ResultCode != EOS_Success) {
                        reports_->achievement_failed("", "Epic couldn't load the player's "
                                                         "achievements: " +
                                                             result_text(info.ResultCode));
                        return;
                    }
                    reports_->achievements(read_progress(wanted));
                }),
            finish<EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo>);
    }

    std::vector<services::AchievementProgress>
    read_progress(const std::vector<services::AchievementUnit>& wanted) {
        std::vector<services::AchievementProgress> progress;
        for (const services::AchievementUnit& unit : wanted) {
            EOS_Achievements_CopyPlayerAchievementByAchievementIdOptions options{};
            options.ApiVersion = EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYACHIEVEMENTID_API_LATEST;
            options.TargetUserId = user_;
            options.LocalUserId = user_;
            options.AchievementId = unit.achievement.c_str();
            EOS_Achievements_PlayerAchievement* achievement = nullptr;
            const EOS_EResult copied = EOS_Achievements_CopyPlayerAchievementByAchievementId(
                achievements(), &options, &achievement);
            if (copied != EOS_Success) {
                reports_->achievement_failed(unit.achievement,
                                             "Epic has no achievement with the id \"" +
                                                 unit.achievement + "\" (" + result_text(copied) +
                                                 "); give it the game's name in the Portal");
                continue;
            }
            services::AchievementProgress done;
            done.achievement = unit.achievement;
            done.unlocked =
                achievement->UnlockTime != EOS_ACHIEVEMENTS_ACHIEVEMENT_UNLOCKTIME_UNDEFINED;
            done.steps = done.unlocked
                             ? unit.steps
                             : static_cast<int>(std::lround(achievement->Progress * unit.steps));
            EOS_Achievements_PlayerAchievement_Release(achievement);
            if (done.unlocked || done.steps > 0) {
                progress.push_back(std::move(done));
            }
        }
        return progress;
    }

    // Adds amount to the player's stat, then calls done with EOS's answer.
    void ingest(const std::string& stat, std::int32_t amount,
                std::function<void(EOS_EResult)> done) {
        EOS_Stats_IngestData data{};
        data.ApiVersion = EOS_STATS_INGESTDATA_API_LATEST;
        data.StatName = stat.c_str();
        data.IngestAmount = amount;
        EOS_Stats_IngestStatOptions options{};
        options.ApiVersion = EOS_STATS_INGESTSTAT_API_LATEST;
        options.LocalUserId = user_;
        options.TargetUserId = user_;
        options.Stats = &data;
        options.StatsCount = 1;
        EOS_Stats_IngestStat(
            EOS_Platform_GetStatsInterface(platform_), &options,
            call<EOS_Stats_IngestStatCompleteCallbackInfo>(
                [done = std::move(done)](const EOS_Stats_IngestStatCompleteCallbackInfo& info) {
                    done(info.ResultCode);
                }),
            finish<EOS_Stats_IngestStatCompleteCallbackInfo>);
    }

    // The best count of the ranks EOS loaded, with the player's own and how many there are.
    [[nodiscard]] services::Scores read_ranks(int count) const {
        services::Scores scores;
        EOS_Leaderboards_GetLeaderboardRecordCountOptions counting{};
        counting.ApiVersion = EOS_LEADERBOARDS_GETLEADERBOARDRECORDCOUNT_API_LATEST;
        const std::uint32_t records =
            EOS_Leaderboards_GetLeaderboardRecordCount(leaderboards(), &counting);
        scores.total = records;
        const std::uint32_t wanted = std::min(records, static_cast<std::uint32_t>(count));
        for (std::uint32_t i = 0; i < wanted; ++i) {
            EOS_Leaderboards_CopyLeaderboardRecordByIndexOptions options{};
            options.ApiVersion = EOS_LEADERBOARDS_COPYLEADERBOARDRECORDBYINDEX_API_LATEST;
            options.LeaderboardRecordIndex = i;
            EOS_Leaderboards_LeaderboardRecord* record = nullptr;
            if (EOS_Leaderboards_CopyLeaderboardRecordByIndex(leaderboards(), &options, &record) ==
                EOS_Success) {
                scores.entries.push_back(score_of(*record));
                EOS_Leaderboards_LeaderboardRecord_Release(record);
            }
        }
        EOS_Leaderboards_CopyLeaderboardRecordByUserIdOptions mine{};
        mine.ApiVersion = EOS_LEADERBOARDS_COPYLEADERBOARDRECORDBYUSERID_API_LATEST;
        mine.UserId = user_;
        EOS_Leaderboards_LeaderboardRecord* record = nullptr;
        if (EOS_Leaderboards_CopyLeaderboardRecordByUserId(leaderboards(), &mine, &record) ==
            EOS_Success) {
            scores.player = score_of(*record);
            EOS_Leaderboards_LeaderboardRecord_Release(record);
        }
        return scores;
    }

    static services::Score score_of(const EOS_Leaderboards_LeaderboardRecord& record) {
        services::Score score;
        score.rank = record.Rank;
        score.player = id_text(record.UserId);
        score.name = record.UserDisplayName == nullptr ? "" : record.UserDisplayName;
        score.value = record.Score;
        return score;
    }

    bool initialized_ = false;
    EOS_HPlatform platform_ = nullptr;
    EOS_ProductUserId user_ = nullptr;
    std::shared_ptr<services::Reports> reports_;
};

} // namespace

// The package's register function: Epic Online Services are the game's games backend.
void register_backends(slash::services::Backends& backends) {
    backends.games = std::make_unique<EpicGames>();
}

} // namespace slash_eos
