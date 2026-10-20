// Glue code: the part of an SDK package that connects the vendor's SDK to the engine. Exports
// compile it into the game with the SDK's headers and defines, and sdk.json's "register" names
// the function below, which adds an analytics backend that hands the game's events to the SDK.
//
// Exports compile it with SLASH_SDK_HELLO defined; the guard keeps tools that read every source,
// such as clang-tidy, from compiling it without the SDK.
#ifdef SLASH_SDK_HELLO

#include "hello.h"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/services/services.hpp"

#include <memory>
#include <string_view>

namespace {

class HelloAnalytics final : public slash::services::AnalyticsBackend {
public:
    [[nodiscard]] std::string_view name() const override { return "hello"; }

    void start(const std::shared_ptr<slash::services::Reports>& /*reports*/) override {
        // A vendor SDK starts with the game's key, which exports pass in as a define.
        hello_init(HELLO_APP_ID);
    }

    void log_event(std::string_view event, const slash::json::Value& params) override {
        // The stand-in SDK has no analytics call, so the event goes to the log.
        slash::log::info("hello analytics event",
                         {{"event", event},
                          {"params", slash::json::write(params, slash::json::Style::Compact)},
                          {"sdk", hello_greeting()}});
    }
};

} // namespace

namespace hello {

void register_backends(slash::services::Backends& backends) {
    backends.analytics.push_back(std::make_unique<HelloAnalytics>());
}

} // namespace hello

#endif // SLASH_SDK_HELLO
