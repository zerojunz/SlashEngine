#include "slash/l10n/locale.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::l10n {
namespace {

// The tables of Godot 4.6's core/string/locales.h that reading locales needs.

// Windows' locale codes that don't follow ISO 639-1, and "C", an unset locale.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kLocaleRenames{{
    {"in", "id"}, // Indonesian
    {"iw", "he"}, // Hebrew
    {"no", "nb"}, // Norwegian Bokmål
    {"C", "en"},  // Locale is not set, fallback to English.
}};

// A language's script, the country it is mostly used in with that script, and the countries
// that use that script.
struct LocaleScript {
    std::string_view language;
    std::string_view script;
    std::string_view default_country;
    std::string_view countries; // separated by ","
};
constexpr std::array<LocaleScript, 18> kLocaleScripts{{
    {"az", "Latn", "", "AZ"},
    {"az", "Arab", "", "IR"},
    {"bs", "Latn", "", "BA"},
    {"ff", "Latn", "", "BF,CM,GH,GM,GN,GW,LR,MR,NE,NG,SL,SN"},
    {"pa", "Arab", "PK", "PK"},
    {"pa", "Guru", "IN", "IN"},
    {"sd", "Arab", "PK", "PK"},
    {"sd", "Deva", "IN", "IN"},
    {"shi", "Tfng", "", "MA"},
    {"sr", "Cyrl", "", "BA,RS,XK"},
    {"sr", "Latn", "", "ME"},
    {"uz", "Latn", "", "UZ"},
    {"uz", "Arab", "AF", "AF"},
    {"vai", "Vaii", "", "LR"},
    {"yue", "Hans", "CN", "CN"},
    {"yue", "Hant", "HK", "HK"},
    {"zh", "Hans", "CN", "CN,SG"},
    {"zh", "Hant", "TW", "HK,MO,TW"},
}};

// Outdated, temporary or exceptionally reserved country codes.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kCountryRenames{{
    {"BU", "MM"}, // Burma, name changed to Myanmar.
    {"KV", "XK"}, // Kosovo (temporary FIPS code to European Commission code).
    {"TP", "TL"}, // East Timor, name changed to Timor-Leste.
    {"UK", "GB"}, // United Kingdom, exceptionally reserved code.
}};

// Regional variants and the language each belongs to.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kLocaleVariants{{
    {"valencia", "ca"},
    {"iqtelif", "tt"},
    {"saaho", "aa"},
    {"tradnl", "es"},
}};

// ISO 15924 script codes, as Godot's script_list (its names left out).
constexpr std::string_view kScripts =
    "Adlm Afak Aghb Ahom Arab Armi Armn Avst Bali Bamu Bass Batk Beng Bhks Blis Bopo Brah Brai "
    "Bugi Buhd Cakm Cans Cari Cham Cher Chrs Cirt Copt Cpmn Cprt Cyrl Deva Diak Dogr Dsrt Dupl "
    "Egyd Egyh Egyp Elba Elym Ethi Geok Geor Glag Gong Gonm Goth Gran Grek Gujr Guru Hang Hani "
    "Hano Hans Hant Hatr Hebr Hira Hluw Hmng Hmnp Hung Inds Ital Java Jurc Kali Kana Kawi Khar "
    "Khmr Khoj Kitl Kits Knda Kpel Kthi Lana Laoo Latn Leke Lepc Limb Lina Linb Lisu Loma Lyci "
    "Lydi Mahj Maka Mand Mani Marc Maya Medf Mend Merc Mero Mlym Modi Mong Moon Mroo Mtei Mult "
    "Mymr Nagm Nand Narb Nbat Newa Nkdb Nkgb Nkoo Nshu Ogam Olck Orkh Orya Osge Osma Ougr Palm "
    "Pauc Pcun Pelm Perm Phag Phli Phlp Phlv Phnx Piqd Plrd Prti Psin Ranj Rjng Rohg Roro Runr "
    "Samr Sara Sarb Saur Sgnw Shaw Shrd Shui Sidd Sind Sinh Sogd Sogo Sora Soyo Sund Sylo Syrc "
    "Tagb Takr Tale Talu Taml Tang Tavt Telu Teng Tfng Tglg Thaa Thai Tibt Tirh Tnsa Toto Ugar "
    "Vaii Visp Vith Wara Wcho Wole Xpeo Xsux Yezi Yiii Zanb";

// Gettext plural rules by locale, from gettext's plural-table.c and CLDR 47's plurals.xml.
constexpr std::array<std::pair<std::string_view, std::string_view>, 30> kPluralRules{{
    {"bm bo dz hnj id ig ii in ja jbo jv jw kde kea km ko lkt lo ms my nqo osa root sah ses sg su "
     "th to tpi vi wo yo yue zh",
     "nplurals=1; plural=0;"},
    {"af an asa ast az bal bem bez bg brx ca ce cgg chr ckb da de dv ee el en eo es et eu fi fo "
     "fur fy gl gsw ha haw he hu ia io it jgo ji jmc ka kaj kcg kk kkj kl ks ksb ku ky lb lg lij "
     "mas mgo ml mn mr nah nb nd ne nl nn nnh no nr ny nyn om or os pap ps pt rm rof rwk saq sc "
     "sd sdh seh sn so sq ss ssy st sv sw syr ta te teo tig tk tn tr ts ug ur uz ve vo vun wae "
     "xh xog yi",
     "nplurals=2; plural=(n != 1);"},
    {"ak bho csw ff fr guw hy kab ln mg nso pa pt_BR si ti wa", "nplurals=2; plural=(n > 1);"},
    {"is mk", "nplurals=2; plural=(n%10==1 && n%100!=11);"},
    {"tzm", "nplurals=2; plural=(n<=1 || (n>=11 && n<=99));"},
    {"am as bn doi fa gu hi kn pcm zu", "nplurals=2; plural=(n==0 || n==1);"},
    {"ceb fil tl", "nplurals=2; plural=(n==1 || n==2 || n==3 || (n%10!=4 && n%10!=6 && n%10!=9));"},
    {"prg",
     "nplurals=3; plural=(n%10==0 || (n%100>=11 && n%100<=19) ? 0 : n%10==1 && n%100!=11 ? 1 : "
     "2);"},
    {"lv", "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n != 0 ? 1 : 2);"},
    {"lt",
     "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && (n%100<10 || n%100>=20) ? 1 : "
     "2);"},
    {"be hr ru sr uk",
     "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || "
     "n%100>=20) ? 1 : 2);"},
    {"bs sh", "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<12 || "
              "n%100>14) ? 1 : 2);"},
    {"lag", "nplurals=3; plural=(n==0 ? 0 : (n==0 || n==1) && n!=0 ? 1 : 2);"},
    {"blo ksh", "nplurals=3; plural=(n==0 ? 0 : n==1 ? 1 : 2);"},
    {"shi", "nplurals=3; plural=(n==0 || n==1 ? 0 : n>=2 && n<=10 ? 1 : 2);"},
    {"pl", "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : "
           "2);"},
    {"mo", "nplurals=3; plural=(n==1 ? 0 : n==0 || (n!=1 && n%100>=1 && n%100<=19) ? 1 : 2);"},
    {"iu iw naq sat se sma smi smj smn sms", "nplurals=3; plural=(n==1 ? 0 : n==2 ? 1 : 2);"},
    {"cs sk", "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;"},
    {"ro", "nplurals=3; plural=n==1 ? 0 : (n==0 || (n%100 > 0 && n%100 < 20)) ? 1 : 2;"},
    {"ga", "nplurals=3; plural=n==1 ? 0 : n==2 ? 1 : 2;"},
    {"sl", "nplurals=4; plural=(n%100==1 ? 0 : n%100==2 ? 1 : n%100==3 || n%100==4 ? 2 : 3);"},
    {"dsb hsb", "nplurals=4; plural=(n%100==1 ? 0 : n%100==2 ? 1 : n%100>=3 && n%100<=4 ? 2 : 3);"},
    {"gv", "nplurals=4; plural=(n%10==1 ? 0 : n%10==2 ? 1 : n%100==0 || n%100==20 || n%100==40 || "
           "n%100==60 || n%100==80 ? 2 : 3);"},
    {"gd",
     "nplurals=4; plural=(n==1 || n==11 ? 0 : n==2 || n==12 ? 1 : (n>=3 && n<=10) || (n>=13 && "
     "n<=19) ? 2 : 3);"},
    {"br",
     "nplurals=5; plural=(n%10==1 && n%100!=11 && n%100!=71 && n%100!=91 ? 0 : n%10==2 && "
     "n%100!=12 && n%100!=72 && n%100!=92 ? 1 : ((n%10>=3 && n%10<=4) || n%10==9) && (n%100<10 "
     "|| n%100>19) && (n%100<70 || n%100>79) && (n%100<90 || n%100>99) ? 2 : n!=0 && "
     "n%1000000==0 ? 3 : 4);"},
    {"mt", "nplurals=5; plural=(n==1 ? 0 : n==2 ? 1 : n==0 || (n%100>=3 && n%100<=10) ? 2 : "
           "n%100>=11 && n%100<=19 ? 3 : 4);"},
    {"kw",
     "nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n%100==2 || n%100==22 || n%100==42 || "
     "n%100==62 || n%100==82 || (n%1000==0 && ((n%100000>=1000 && n%100000<=20000) || "
     "n%100000==40000 || n%100000==60000 || n%100000==80000)) || (n!=0 && n%1000000==100000) ? "
     "2 : n%100==3 || n%100==23 || n%100==43 || n%100==63 || n%100==83 ? 3 : n!=1 && (n%100==1 "
     "|| n%100==21 || n%100==41 || n%100==61 || n%100==81) ? 4 : 5);"},
    {"ar ars", "nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : "
               "n%100>=11 && n%100<=99 ? 4 : 5);"},
    {"cy", "nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n==3 ? 3 : n==6 ? 4 : 5);"},
}};

// Parts of text between separators, empty ones included, as Godot's String::split.
std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

bool upper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool lower(char c) {
    return c >= 'a' && c <= 'z';
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (upper(c)) {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

template <std::size_t N>
std::string_view find(const std::array<std::pair<std::string_view, std::string_view>, N>& table,
                      std::string_view key) {
    for (const auto& [from, to] : table) {
        if (from == key) {
            return to;
        }
    }
    return {};
}

bool known_script(std::string_view script) {
    if (script.size() != 4) {
        return false;
    }
    return std::ranges::any_of(split(kScripts, ' '),
                               [&](std::string_view code) { return code == script; });
}

// The variant a part names, if it is one of the language's.
std::string variant_of(std::string_view part, std::string_view language) {
    const std::string name = to_lower(part);
    const std::string_view of = find(kLocaleVariants, name);
    return !of.empty() && of == language ? name : std::string();
}

} // namespace

Locale Locale::parse(std::string_view text, bool add_defaults) {
    // Replaces '-' with '_' for macOS style locales.
    std::string universal(text);
    std::ranges::replace(universal, '-', '_');
    const std::string_view whole = universal;
    const std::size_t at = whole.find('@');
    const std::string_view before = whole.substr(0, at);

    Locale out;
    const std::vector<std::string_view> elements = split(before, '_');
    out.language = elements[0];
    if (elements.size() >= 2) {
        const std::string_view second = elements[1];
        if (second.size() == 4 && upper(second[0]) && lower(second[1]) && lower(second[2]) &&
            lower(second[3])) {
            out.script = second;
        }
        if (second.size() == 2 && upper(second[0]) && upper(second[1])) {
            out.country = second;
        }
    }
    if (elements.size() >= 3) {
        const std::string_view third = elements[2];
        if (third.size() == 2 && upper(third[0]) && upper(third[1])) {
            out.country = third;
        } else if (std::string variant = variant_of(third, out.language); !variant.empty()) {
            out.variant = std::move(variant);
        }
    }
    if (elements.size() >= 4) {
        if (std::string variant = variant_of(elements[3], out.language); !variant.empty()) {
            out.variant = std::move(variant);
        }
    }

    // Try extract script and variant from the extra part.
    if (at != std::string_view::npos) {
        std::string_view extra = whole.substr(at + 1);
        extra = extra.substr(0, extra.find('@'));
        for (const std::string_view each : split(extra, ';')) {
            const std::string name = to_lower(each);
            if (name == "cyrillic") {
                out.script = "Cyrl";
                break;
            }
            if (name == "latin") {
                out.script = "Latn";
                break;
            }
            if (name == "devanagari") {
                out.script = "Deva";
                break;
            }
            if (std::string variant = variant_of(each, out.language); !variant.empty()) {
                out.variant = std::move(variant);
            }
        }
    }

    // Handles known non-ISO language names used e.g. on Windows.
    if (const std::string_view renamed = find(kLocaleRenames, out.language); !renamed.empty()) {
        out.language = renamed;
    }
    if (const std::string_view renamed = find(kCountryRenames, out.country); !renamed.empty()) {
        out.country = renamed;
    }
    // Remove unsupported script codes.
    if (!known_script(out.script)) {
        out.script.clear();
    }

    // Add script code base on language and country codes for some ambiguous cases.
    if (add_defaults) {
        if (out.script.empty()) {
            for (const LocaleScript& info : kLocaleScripts) {
                if (info.language != out.language) {
                    continue;
                }
                const std::vector<std::string_view> countries = split(info.countries, ',');
                if (out.country.empty() ||
                    std::ranges::find(countries, out.country) != countries.end()) {
                    out.script = info.script;
                    break;
                }
            }
        }
        if (!out.script.empty() && out.country.empty()) {
            // Add country code based on script for some ambiguous cases.
            for (const LocaleScript& info : kLocaleScripts) {
                if (info.language == out.language && info.script == out.script) {
                    out.country = info.default_country;
                    break;
                }
            }
        }
    }
    return out;
}

std::string Locale::str() const {
    std::string out = language;
    for (const std::string* part : {&script, &country, &variant}) {
        if (!part->empty()) {
            out += '_';
            out += *part;
        }
    }
    return out;
}

std::string standardize_locale(std::string_view locale, bool add_defaults) {
    return Locale::parse(locale, add_defaults).str();
}

bool is_locale(std::string_view locale) {
    const std::string language = Locale::parse(locale, false).language;
    return language.size() >= 2 && language.size() <= 3 &&
           std::ranges::all_of(language, [](char c) { return lower(c) || upper(c); });
}

int compare_locales(std::string_view a, std::string_view b) {
    if (a == b) {
        return 10; // Exact match.
    }
    // Godot keeps a cache of these; a lookup reads at most a few locales, so this one doesn't.
    const Locale locale_a = Locale::parse(a, true);
    const Locale locale_b = Locale::parse(b, true);
    if (locale_a == locale_b) {
        return 10;
    }
    if (locale_a.language != locale_b.language) {
        return 0;
    }
    // Matching language: each part both locales have makes the score one better or one worse.
    int score = 5;
    const auto weigh = [&score](const std::string& x, const std::string& y) {
        if (!x.empty() && !y.empty()) {
            score += x == y ? 1 : -1;
        }
    };
    weigh(locale_a.script, locale_b.script);
    weigh(locale_a.country, locale_b.country);
    weigh(locale_a.variant, locale_b.variant);
    return score;
}

std::string plural_rules_for(std::string_view locale) {
    static const std::map<std::string, std::string, std::less<>> rules = [] {
        std::map<std::string, std::string, std::less<>> map;
        for (const auto& [locales, rule] : kPluralRules) {
            for (const std::string_view each : split(locales, ' ')) {
                map[std::string(each)] = rule;
            }
        }
        return map;
    }();
    if (const auto found = rules.find(locale); found != rules.end()) {
        return found->second;
    }
    const Locale parsed = Locale::parse(locale, false);
    if (!parsed.country.empty()) {
        if (const auto found = rules.find(parsed.language + "_" + parsed.country);
            found != rules.end()) {
            return found->second;
        }
    }
    if (const auto found = rules.find(parsed.language); found != rules.end()) {
        return found->second;
    }
    return {};
}

bool is_locale_right_to_left(std::string_view locale) {
    const std::string_view language = locale.substr(0, locale.find('_'));
    return language == "ar" || language == "dv" || language == "he" || language == "fa" ||
           language == "ff" || language == "ku" || language == "ur";
}

} // namespace slash::l10n
