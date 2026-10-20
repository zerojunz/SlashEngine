// Backends written in Java (slash/services/bridges.hpp), called through JNI. The game's thread
// is attached to the Java VM by SDL; reports come back on any thread through Reports.report, a
// native method registered when the first backend starts.
//
// Only Android builds compile it (engine/services/CMakeLists.txt); the guard keeps tools that read
// every source, such as clang-tidy, from compiling it elsewhere.
#ifdef __ANDROID__

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/services/bridges.hpp"

#include <SDL3/SDL_system.h>

#include <array>
#include <cstdint>
#include <jni.h>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace slash::services {
namespace {

constexpr const char* kReportsClass = "org.slashengine.services.Reports";

// Reports by the handle a Java Reports object holds, so a report made after its backend is gone
// finds nothing instead of a dangling pointer.
struct Handles {
    std::mutex mutex;
    std::map<std::int64_t, std::weak_ptr<Reports>> reports;
    std::int64_t next = 1;
};

Handles& handles() {
    static Handles instance;
    return instance;
}

std::int64_t add_handle(const std::shared_ptr<Reports>& reports) {
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    const std::int64_t handle = all.next++;
    all.reports[handle] = reports;
    return handle;
}

void remove_handle(std::int64_t handle) {
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    all.reports.erase(handle);
}

std::shared_ptr<Reports> find_handle(std::int64_t handle) {
    Handles& all = handles();
    const std::scoped_lock lock(all.mutex);
    const auto found = all.reports.find(handle);
    return found != all.reports.end() ? found->second.lock() : nullptr;
}

JNIEnv* env() {
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

// Logs and clears a pending Java exception; true if there was one.
bool failed(JNIEnv* jni, std::string_view what) {
    if (jni->ExceptionCheck() == JNI_FALSE) {
        return false;
    }
    jni->ExceptionDescribe();
    jni->ExceptionClear();
    log::error("a Java services backend threw", {{"call", std::string(what)}});
    return true;
}

std::string text(JNIEnv* jni, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = jni->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string out(chars);
    jni->ReleaseStringUTFChars(value, chars);
    return out;
}

// A local reference to a Java string, deleted when it goes.
class JavaString {
public:
    JavaString(JNIEnv* jni, std::string_view value) : jni_(jni) {
        const std::string copy(value);
        string_ = jni->NewStringUTF(copy.c_str());
    }
    JavaString(const JavaString&) = delete;
    JavaString& operator=(const JavaString&) = delete;
    JavaString(JavaString&&) = delete;
    JavaString& operator=(JavaString&&) = delete;
    ~JavaString() {
        if (string_ != nullptr) {
            jni_->DeleteLocalRef(string_);
        }
    }
    [[nodiscard]] jstring get() const { return string_; }

private:
    JNIEnv* jni_;
    jstring string_ = nullptr;
};

// A class from the game's APK, through the activity's class loader, which threads SDL didn't
// start from Java also reach. A global reference, or null.
jclass load_class(JNIEnv* jni, const std::string& name) {
    auto* activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr) {
        log::error("no Android activity to load a Java backend from", {{"class", name}});
        return nullptr;
    }
    jclass activity_class = jni->GetObjectClass(activity);
    const jmethodID get_loader =
        jni->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = jni->CallObjectMethod(activity, get_loader);
    jclass loader_class = jni->FindClass("java/lang/ClassLoader");
    const jmethodID load =
        jni->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jclass found = nullptr;
    {
        const JavaString java_name(jni, name);
        found = static_cast<jclass>(jni->CallObjectMethod(loader, load, java_name.get()));
    }
    jclass out = nullptr;
    if (!failed(jni, "loading " + name) && found != nullptr) {
        out = static_cast<jclass>(jni->NewGlobalRef(found));
        jni->DeleteLocalRef(found);
    } else {
        log::error("could not load a Java services backend", {{"class", name}});
    }
    jni->DeleteLocalRef(loader_class);
    jni->DeleteLocalRef(loader);
    jni->DeleteLocalRef(activity_class);
    jni->DeleteLocalRef(activity);
    return out;
}

Reward reward_of(std::string type, double amount) {
    return {.type = std::move(type), .amount = amount};
}

PurchaseState parse_state(std::string_view name) {
    if (name == "pending") {
        return PurchaseState::Pending;
    }
    if (name == "cancelled") {
        return PurchaseState::Cancelled;
    }
    if (name == "failed") {
        return PurchaseState::Failed;
    }
    if (name == "restored") {
        return PurchaseState::Restored;
    }
    return PurchaseState::Purchased;
}

std::string field(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

void report_products(Reports& reports, const std::string& text) {
    auto parsed = json::parse(text);
    if (!parsed || !parsed.value().is_array()) {
        reports.products_failed("the backend's products are not a JSON array");
        return;
    }
    std::vector<Product> products;
    for (const json::Value& item : parsed.value().as_array()) {
        if (!item.is_object()) {
            continue;
        }
        Product product;
        product.id = field(item, "id");
        product.title = field(item, "title");
        product.description = field(item, "description");
        product.price = field(item, "price");
        product.currency = field(item, "currency");
        if (const json::Value* micros = item.find("price_micros");
            micros != nullptr && micros->is_number()) {
            product.price_micros = micros->as_int();
        }
        const json::Value* available = item.find("available");
        product.available = available == nullptr || !available->is_bool() || available->as_bool();
        products.push_back(std::move(product));
    }
    reports.products(std::move(products));
}

std::int64_t number(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return found != nullptr && found->is_number() ? found->as_int() : 0;
}

void report_achievements(Reports& reports, const std::string& text) {
    auto parsed = json::parse(text);
    if (!parsed || !parsed.value().is_array()) {
        reports.achievement_failed({}, "the backend's achievements are not a JSON array");
        return;
    }
    std::vector<AchievementProgress> progress;
    for (const json::Value& item : parsed.value().as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const json::Value* unlocked = item.find("unlocked");
        progress.push_back(
            {.achievement = field(item, "achievement"),
             .steps = static_cast<int>(number(item, "steps")),
             .unlocked = unlocked != nullptr && unlocked->is_bool() && unlocked->as_bool()});
    }
    reports.achievements(std::move(progress));
}

Score score_of(const json::Value& item) {
    return {.rank = number(item, "rank"),
            .player = field(item, "player"),
            .name = field(item, "name"),
            .value = number(item, "score")};
}

void report_scores(Reports& reports, const std::string& leaderboard, const std::string& text) {
    auto parsed = json::parse(text);
    if (!parsed || !parsed.value().is_object()) {
        reports.scores_failed(leaderboard, "the backend's scores are not a JSON object");
        return;
    }
    const json::Value& object = parsed.value();
    Scores scores;
    if (const json::Value* entries = object.find("entries");
        entries != nullptr && entries->is_array()) {
        for (const json::Value& item : entries->as_array()) {
            if (item.is_object()) {
                scores.entries.push_back(score_of(item));
            }
        }
    }
    if (const json::Value* player = object.find("player");
        player != nullptr && player->is_object()) {
        scores.player = score_of(*player);
    }
    scores.total = number(object, "total");
    reports.scores(leaderboard, std::move(scores));
}

void report_purchase(Reports& reports, const std::string& text) {
    auto parsed = json::parse(text);
    if (!parsed || !parsed.value().is_object()) {
        return;
    }
    const json::Value& item = parsed.value();
    reports.purchase({.product = field(item, "product"),
                      .transaction = field(item, "transaction"),
                      .state = parse_state(field(item, "state")),
                      .receipt = field(item, "receipt"),
                      .error = field(item, "error")});
}

// The games service's reports: the leaderboard, achievement or player's ID comes as placement.
void report_games(Reports& reports, const std::string& kind, const std::string& name,
                  std::string message) {
    if (kind == "signed_in") {
        reports.signed_in({.id = name, .name = std::move(message)});
    } else if (kind == "sign_in_failed") {
        reports.sign_in_failed(message);
    } else if (kind == "achievements") {
        report_achievements(reports, message);
    } else if (kind == "achievement_failed") {
        reports.achievement_failed(name, message);
    } else if (kind == "score_submitted") {
        const auto score = json::parse(message);
        reports.score_submitted(name,
                                score && score.value().is_number() ? score.value().as_int() : 0);
    } else if (kind == "score_failed") {
        reports.score_failed(name, message);
    } else if (kind == "scores") {
        report_scores(reports, name, message);
    } else if (kind == "scores_failed") {
        reports.scores_failed(name, message);
    } else if (kind == "games_ui_closed") {
        reports.games_ui_closed();
    }
}

// Reports.report(long handle, String kind, String placement, String text, double amount).
void JNICALL native_report(JNIEnv* jni, jclass /*reports_class*/, jlong handle, jstring kind_value,
                           jstring placement_value, jstring text_value, jdouble amount) {
    const std::shared_ptr<Reports> reports = find_handle(handle);
    if (reports == nullptr) {
        return;
    }
    const std::string kind = text(jni, kind_value);
    const std::string placement = text(jni, placement_value);
    std::string message = text(jni, text_value);
    if (kind == "ad_loaded") {
        reports->ad_loaded(placement);
    } else if (kind == "ad_failed") {
        reports->ad_failed(placement, message);
    } else if (kind == "ad_shown") {
        reports->ad_shown(placement);
    } else if (kind == "ad_rewarded") {
        reports->ad_rewarded(placement, reward_of(std::move(message), amount));
    } else if (kind == "ad_closed") {
        reports->ad_closed(placement);
    } else if (kind == "products") {
        report_products(*reports, message);
    } else if (kind == "products_failed") {
        reports->products_failed(message);
    } else if (kind == "purchase") {
        report_purchase(*reports, message);
    } else if (kind == "restore_finished") {
        reports->restore_finished(message);
    } else {
        report_games(*reports, kind, placement, message);
    }
}

// The Reports class with its native method registered, and its constructor.
struct ReportsClass {
    jclass java_class = nullptr;
    jmethodID constructor = nullptr;
};

const ReportsClass& reports_class(JNIEnv* jni) {
    static const ReportsClass loaded = [jni] {
        ReportsClass out;
        out.java_class = load_class(jni, kReportsClass);
        if (out.java_class == nullptr) {
            return out;
        }
        const std::array<JNINativeMethod, 1> methods{
            {{"report", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;D)V",
              reinterpret_cast<void*>(&native_report)}}};
        if (jni->RegisterNatives(out.java_class, methods.data(),
                                 static_cast<jint>(methods.size())) != JNI_OK) {
            (void)failed(jni, "registering Reports.report");
        }
        out.constructor = jni->GetMethodID(out.java_class, "<init>", "(J)V");
        return out;
    }();
    return loaded;
}

std::string consent_text(Consent consent) {
    return std::string(consent_name(consent));
}

// A Java method's arguments and the signature they make, the strings local references that
// outlive the call.
class JavaArgs {
public:
    explicit JavaArgs(JNIEnv* jni) : jni_(jni) {}

    JavaArgs& text(std::string_view value) {
        strings_.push_back(std::make_unique<JavaString>(jni_, value));
        jvalue arg{};
        arg.l = strings_.back()->get();
        add("Ljava/lang/String;", arg);
        return *this;
    }
    JavaArgs& integer(int value) {
        jvalue arg{};
        arg.i = static_cast<jint>(value);
        add("I", arg);
        return *this;
    }
    JavaArgs& wide(std::int64_t value) {
        jvalue arg{};
        arg.j = static_cast<jlong>(value);
        add("J", arg);
        return *this;
    }
    JavaArgs& flag(bool value) {
        jvalue arg{};
        arg.z = value ? JNI_TRUE : JNI_FALSE;
        add("Z", arg);
        return *this;
    }

    [[nodiscard]] const std::string& signature() const { return signature_; }
    [[nodiscard]] const jvalue* values() const { return values_.data(); }

private:
    void add(const char* type, jvalue value) {
        signature_ += type;
        values_.push_back(value);
    }

    JNIEnv* jni_;
    std::string signature_;
    std::vector<jvalue> values_;
    std::vector<std::unique_ptr<JavaString>> strings_;
};

// One Java object implementing one of the interfaces, and its methods.
class JavaObject {
public:
    explicit JavaObject(std::string class_name)
        : class_name_(std::move(class_name)), name_(class_name_) {}
    JavaObject(const JavaObject&) = delete;
    JavaObject& operator=(const JavaObject&) = delete;
    JavaObject(JavaObject&&) = delete;
    JavaObject& operator=(JavaObject&&) = delete;
    ~JavaObject() {
        if (handle_ != 0) {
            remove_handle(handle_);
        }
        JNIEnv* jni = env();
        if (jni == nullptr) {
            return;
        }
        if (object_ != nullptr) {
            jni->DeleteGlobalRef(object_);
        }
        if (class_ != nullptr) {
            jni->DeleteGlobalRef(class_);
        }
    }

    // Makes the object and calls its start; false if it couldn't be made.
    bool start(const std::shared_ptr<Reports>& reports) {
        reports_ = reports;
        JNIEnv* jni = env();
        if (jni == nullptr) {
            return false;
        }
        class_ = load_class(jni, class_name_);
        if (class_ == nullptr) {
            return false;
        }
        const jmethodID constructor = jni->GetMethodID(class_, "<init>", "()V");
        if (failed(jni, class_name_ + "()") || constructor == nullptr) {
            return false;
        }
        jobject made = jni->NewObject(class_, constructor);
        if (failed(jni, class_name_ + "()") || made == nullptr) {
            return false;
        }
        object_ = jni->NewGlobalRef(made);
        jni->DeleteLocalRef(made);
        jstring name = static_cast<jstring>(
            jni->CallObjectMethod(object_, method("name", "()Ljava/lang/String;")));
        if (!failed(jni, "name") && name != nullptr) {
            if (std::string java_name = text(jni, name); !java_name.empty()) {
                name_ = std::move(java_name);
            }
            jni->DeleteLocalRef(name);
        }
        const ReportsClass& reports_type = reports_class(jni);
        if (reports_type.java_class == nullptr || reports_type.constructor == nullptr) {
            return false;
        }
        handle_ = add_handle(reports);
        jobject java_reports = jni->NewObject(reports_type.java_class, reports_type.constructor,
                                              static_cast<jlong>(handle_));
        auto* activity = static_cast<jobject>(SDL_GetAndroidActivity());
        jni->CallVoidMethod(
            object_, method("start", "(Landroid/app/Activity;Lorg/slashengine/services/Reports;)V"),
            activity, java_reports);
        (void)failed(jni, "start");
        if (activity != nullptr) {
            jni->DeleteLocalRef(activity);
        }
        if (java_reports != nullptr) {
            jni->DeleteLocalRef(java_reports);
        }
        return true;
    }

    [[nodiscard]] std::string_view name() const { return name_; }
    // Where to report for a backend that couldn't start.
    [[nodiscard]] std::shared_ptr<Reports> reports() const { return reports_.lock(); }

    // Calls a void method with any arguments.
    void call(const char* method_name, const JavaArgs& args) {
        JNIEnv* jni = env();
        if (jni == nullptr || object_ == nullptr) {
            return;
        }
        const std::string signature = "(" + args.signature() + ")V";
        const jmethodID id = method(method_name, signature.c_str());
        if (id == nullptr) {
            return;
        }
        jni->CallVoidMethodA(object_, id, args.values());
        (void)failed(jni, method_name);
    }

    // Calls a void method whose parameters are all strings.
    template <typename... Texts>
    void call(const char* method_name, const Texts&... texts) {
        JNIEnv* jni = env();
        if (jni == nullptr || object_ == nullptr) {
            return;
        }
        std::string signature = "(";
        for (std::size_t i = 0; i < sizeof...(Texts); ++i) {
            signature += "Ljava/lang/String;";
        }
        signature += ")V";
        const jmethodID id = method(method_name, signature.c_str());
        if (id == nullptr) {
            return;
        }
        // Each argument a local reference that outlives the call.
        const std::array<std::unique_ptr<JavaString>, sizeof...(Texts)> args{
            std::make_unique<JavaString>(jni, texts)...};
        call_with(jni, id, args, std::make_index_sequence<sizeof...(Texts)>{});
        (void)failed(jni, method_name);
    }

private:
    template <std::size_t N, std::size_t... I>
    void call_with(JNIEnv* jni, jmethodID id,
                   const std::array<std::unique_ptr<JavaString>, N>& args,
                   std::index_sequence<I...> /*indices*/) {
        jni->CallVoidMethod(object_, id, args[I]->get()...);
    }

    jmethodID method(const char* method_name, const char* signature) {
        JNIEnv* jni = env();
        const jmethodID id = jni->GetMethodID(class_, method_name, signature);
        if (failed(jni, std::string(method_name) + signature)) {
            return nullptr;
        }
        return id;
    }

    std::string class_name_;
    std::string name_;
    std::weak_ptr<Reports> reports_;
    jclass class_ = nullptr;
    jobject object_ = nullptr;
    std::int64_t handle_ = 0;
};

class JavaAds final : public AdsBackend {
public:
    explicit JavaAds(std::string_view class_name) : object_(std::string(class_name)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<Reports>& reports) override {
        started_ = object_.start(reports);
    }
    void set_consent(const ConsentState& consent) override {
        if (started_) {
            object_.call("setConsent", consent_text(consent.analytics),
                         consent_text(consent.personalized_ads));
        }
    }
    void load(const AdUnit& unit) override { call("load", unit); }
    void show(const AdUnit& unit) override { call("show", unit); }
    void hide(const AdUnit& unit) override { call("hide", unit); }

private:
    void call(const char* method_name, const AdUnit& unit) {
        if (started_) {
            object_.call(method_name, unit.placement, std::string(format_name(unit.format)),
                         unit.id);
        } else if (const auto reports = object_.reports();
                   reports != nullptr && method_name != std::string_view("hide")) {
            reports->ad_failed(unit.placement, "the ads Java backend could not start");
        }
    }

    JavaObject object_;
    bool started_ = false;
};

class JavaAnalytics final : public AnalyticsBackend {
public:
    explicit JavaAnalytics(std::string_view class_name) : object_(std::string(class_name)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<Reports>& reports) override {
        started_ = object_.start(reports);
    }
    void set_consent(const ConsentState& consent) override {
        if (started_) {
            object_.call("setConsent", consent_text(consent.analytics),
                         consent_text(consent.personalized_ads));
        }
    }
    void log_event(std::string_view event, const json::Value& params) override {
        if (started_) {
            object_.call("logEvent", std::string(event),
                         params.is_object() ? json::write(params, json::Style::Compact)
                                            : std::string("{}"));
        }
    }
    void set_user_property(std::string_view property, std::string_view value) override {
        if (started_) {
            object_.call("setUserProperty", std::string(property), std::string(value));
        }
    }
    void set_user_id(std::string_view id) override {
        if (started_) {
            object_.call("setUserId", std::string(id));
        }
    }
    void flush() override {
        if (started_) {
            object_.call("flush");
        }
    }

private:
    JavaObject object_;
    bool started_ = false;
};

class JavaPurchases final : public PurchasesBackend {
public:
    explicit JavaPurchases(std::string_view class_name) : object_(std::string(class_name)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<Reports>& reports) override {
        started_ = object_.start(reports);
    }
    void set_consent(const ConsentState& consent) override {
        if (started_) {
            object_.call("setConsent", consent_text(consent.analytics),
                         consent_text(consent.personalized_ads));
        }
    }
    void fetch_products(std::span<const Product> catalog) override {
        if (!started_) {
            // A store that can't start is unavailable, as with none.
            if (const auto reports = object_.reports(); reports != nullptr) {
                reports->products_failed("the store's Java backend could not start");
            }
            return;
        }
        json::Array products;
        for (const Product& product : catalog) {
            json::Value item;
            item["id"] = product.id;
            item["type"] = type_name(product.type);
            products.push_back(std::move(item));
        }
        object_.call("fetchProducts",
                     json::write(json::Value(std::move(products)), json::Style::Compact));
    }
    void purchase(const Product& product) override {
        if (started_) {
            object_.call("purchase", product.id, std::string(type_name(product.type)));
        }
    }
    void finish(const Purchase& purchase, const Product& product) override {
        if (started_) {
            object_.call("finish", purchase.product, std::string(type_name(product.type)),
                         purchase.transaction, purchase.receipt);
        }
    }
    void restore() override {
        if (started_) {
            object_.call("restore");
        } else if (const auto reports = object_.reports(); reports != nullptr) {
            reports->restore_finished("the store's Java backend could not start");
        }
    }

private:
    JavaObject object_;
    bool started_ = false;
};

class JavaGames final : public GamesBackend {
public:
    explicit JavaGames(std::string_view class_name) : object_(std::string(class_name)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<Reports>& reports) override {
        started_ = object_.start(reports);
    }
    void set_consent(const ConsentState& consent) override {
        if (started_) {
            object_.call("setConsent", consent_text(consent.analytics),
                         consent_text(consent.personalized_ads));
        }
    }
    void sign_in(bool interactive) override {
        if (started_) {
            object_.call("signIn", JavaArgs(env()).flag(interactive));
        } else if (const auto reports = object_.reports(); reports != nullptr) {
            reports->sign_in_failed("the games Java backend could not start");
        }
    }
    void load_achievements(std::span<const AchievementUnit> units) override {
        if (!started_) {
            return;
        }
        json::Array achievements;
        for (const AchievementUnit& unit : units) {
            json::Value item;
            item["achievement"] = unit.achievement;
            item["id"] = unit.id;
            item["steps"] = unit.steps;
            achievements.push_back(std::move(item));
        }
        object_.call("loadAchievements",
                     json::write(json::Value(std::move(achievements)), json::Style::Compact));
    }
    void unlock(const AchievementUnit& unit) override {
        if (started_) {
            object_.call("unlock", unit.achievement, unit.id);
        }
    }
    void set_steps(const AchievementUnit& unit, int steps) override {
        if (started_) {
            object_.call("setSteps", JavaArgs(env())
                                         .text(unit.achievement)
                                         .text(unit.id)
                                         .integer(steps)
                                         .integer(unit.steps));
        }
    }
    void submit_score(const LeaderboardUnit& unit, std::int64_t score) override {
        if (started_) {
            object_.call("submitScore",
                         JavaArgs(env()).text(unit.leaderboard).text(unit.id).wide(score));
        }
    }
    void load_scores(const LeaderboardUnit& unit, const ScoreQuery& query) override {
        if (!started_) {
            if (const auto reports = object_.reports(); reports != nullptr) {
                reports->scores_failed(unit.leaderboard, "the games Java backend could not start");
            }
            return;
        }
        object_.call("loadScores", JavaArgs(env())
                                       .text(unit.leaderboard)
                                       .text(unit.id)
                                       .text(span_name(query.time))
                                       .text(collection_name(query.collection))
                                       .integer(query.count));
    }
    void show_achievements() override {
        if (started_) {
            object_.call("showAchievements");
        } else if (const auto reports = object_.reports(); reports != nullptr) {
            reports->games_ui_closed();
        }
    }
    void show_leaderboards(const LeaderboardUnit* unit) override {
        if (started_) {
            object_.call("showLeaderboards", unit != nullptr ? unit->leaderboard : std::string(),
                         unit != nullptr ? unit->id : std::string());
        } else if (const auto reports = object_.reports(); reports != nullptr) {
            reports->games_ui_closed();
        }
    }

private:
    JavaObject object_;
    bool started_ = false;
};

} // namespace

std::unique_ptr<AdsBackend> java_ads_backend(std::string_view class_name) {
    return std::make_unique<JavaAds>(class_name);
}

std::unique_ptr<AnalyticsBackend> java_analytics_backend(std::string_view class_name) {
    return std::make_unique<JavaAnalytics>(class_name);
}

std::unique_ptr<PurchasesBackend> java_purchases_backend(std::string_view class_name) {
    return std::make_unique<JavaPurchases>(class_name);
}

std::unique_ptr<GamesBackend> java_games_backend(std::string_view class_name) {
    return std::make_unique<JavaGames>(class_name);
}

} // namespace slash::services

#endif // __ANDROID__
