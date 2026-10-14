#include "slash/core/reflect.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

namespace game {

struct Stats {
    int strength = 1;
    float speed = 2.5F;
};
SLASH_REFLECT(Stats, strength, speed)

struct Hero {
    std::string name = "hero";
    Stats stats;
    bool alive = true;
};
SLASH_COMPONENT(Hero, name, stats, alive)

struct Marker {};
SLASH_COMPONENT(Marker)

struct Plain {
    int value = 0;
};

} // namespace game

namespace reflect = slash::reflect;

static_assert(reflect::Reflected<game::Hero>);
static_assert(reflect::Reflected<game::Stats>);
static_assert(!reflect::Reflected<game::Plain>);
static_assert(reflect::Component<game::Hero>);
static_assert(reflect::Component<game::Marker>);
static_assert(!reflect::Component<game::Stats>);
static_assert(reflect::type_name<game::Hero>() == "Hero");
static_assert(reflect::field_count<game::Hero>() == 3);
static_assert(reflect::field_count<game::Marker>() == 0);

TEST_CASE("for_each_field visits fields in declaration order") {
    std::vector<std::string_view> names;
    reflect::for_each_field<game::Hero>([&](const auto& field) { names.push_back(field.name); });
    CHECK(names == std::vector<std::string_view>{"name", "stats", "alive"});
}

TEST_CASE("fields read and write through member pointers") {
    game::Stats stats;
    reflect::for_each_field<game::Stats>([&](const auto& field) { field.get(stats) += 1; });
    CHECK(stats.strength == 2);
    CHECK(stats.speed == 3.5F);
}
