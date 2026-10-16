#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

// Compile-time reflection for plain structs.
//
// One macro, placed after the struct in the same namespace, lists the fields:
//
//   struct Health {
//       float current = 100.0F;
//       float max = 100.0F;
//   };
//   SLASH_COMPONENT(Health, current, max)
//
// SLASH_COMPONENT marks an ECS component; SLASH_REFLECT is the same for value types used inside
// components. That registration drives JSON serialization, the generated JSON Schema and (later)
// the command API, so no glue code is written by hand.
namespace slash::reflect {

template <typename T>
struct Tag {
    using type = T;
};

template <typename Class, typename Member>
struct Field {
    using class_type = Class;
    using member_type = Member;

    std::string_view name;
    Member Class::*pointer;

    [[nodiscard]] constexpr Member& get(Class& object) const { return object.*pointer; }
    [[nodiscard]] constexpr const Member& get(const Class& object) const { return object.*pointer; }
};

template <typename Class, typename... Members>
struct TypeInfo {
    std::string_view name;
    bool component = false;
    std::tuple<Field<Class, Members>...> fields;
};

// A field of Class, which may be declared in a base of Class: CheckBox reflects Button's fields.
template <typename Class, typename Owner, typename Member>
constexpr Field<Class, Member> make_field(std::string_view name, Member Owner::*pointer) {
    return {name, pointer};
}

template <typename Class, typename... Members>
constexpr TypeInfo<Class, Members...> make_type(std::string_view name, bool component,
                                                Field<Class, Members>... fields) {
    return {name, component, std::tuple<Field<Class, Members>...>(fields...)};
}

// Found by argument-dependent lookup on Tag<T>, so the macro works in any namespace.
template <typename T>
concept Reflected = requires { slash_reflect(Tag<T>{}); };

template <Reflected T>
constexpr auto info() {
    return slash_reflect(Tag<T>{});
}

template <typename T>
concept Component = Reflected<T> && info<T>().component;

template <Reflected T>
constexpr std::string_view type_name() {
    return info<T>().name;
}

template <Reflected T>
constexpr std::size_t field_count() {
    return std::tuple_size_v<decltype(info<T>().fields)>;
}

// Calls fn(field) for each reflected field of T, in declaration order.
template <Reflected T, typename Fn>
constexpr void for_each_field(Fn&& fn) {
    std::apply([&](const auto&... field) { (fn(field), ...); }, info<T>().fields);
}

} // namespace slash::reflect

// Expands macro(data, arg) for each argument. Uses __VA_OPT__, so MSVC needs /Zc:preprocessor,
// which slash::core adds to its users.
#define SLASH_DETAIL_PARENS ()
#define SLASH_DETAIL_EXPAND(...)                                                                   \
    SLASH_DETAIL_EXPAND4(                                                                          \
        SLASH_DETAIL_EXPAND4(SLASH_DETAIL_EXPAND4(SLASH_DETAIL_EXPAND4(__VA_ARGS__))))
#define SLASH_DETAIL_EXPAND4(...)                                                                  \
    SLASH_DETAIL_EXPAND3(                                                                          \
        SLASH_DETAIL_EXPAND3(SLASH_DETAIL_EXPAND3(SLASH_DETAIL_EXPAND3(__VA_ARGS__))))
#define SLASH_DETAIL_EXPAND3(...)                                                                  \
    SLASH_DETAIL_EXPAND2(                                                                          \
        SLASH_DETAIL_EXPAND2(SLASH_DETAIL_EXPAND2(SLASH_DETAIL_EXPAND2(__VA_ARGS__))))
#define SLASH_DETAIL_EXPAND2(...)                                                                  \
    SLASH_DETAIL_EXPAND1(                                                                          \
        SLASH_DETAIL_EXPAND1(SLASH_DETAIL_EXPAND1(SLASH_DETAIL_EXPAND1(__VA_ARGS__))))
#define SLASH_DETAIL_EXPAND1(...) __VA_ARGS__
#define SLASH_DETAIL_FOR_EACH(macro, data, ...)                                                    \
    __VA_OPT__(SLASH_DETAIL_EXPAND(SLASH_DETAIL_FOR_EACH_STEP(macro, data, __VA_ARGS__)))
#define SLASH_DETAIL_FOR_EACH_STEP(macro, data, first, ...)                                        \
    macro(data, first)                                                                             \
        __VA_OPT__(SLASH_DETAIL_FOR_EACH_AGAIN SLASH_DETAIL_PARENS(macro, data, __VA_ARGS__))
#define SLASH_DETAIL_FOR_EACH_AGAIN() SLASH_DETAIL_FOR_EACH_STEP

#define SLASH_DETAIL_FIELD(Type, member)                                                           \
    , ::slash::reflect::make_field<Type>(#member, &Type::member)

#define SLASH_DETAIL_REFLECT(Type, is_component, ...)                                              \
    [[maybe_unused]] constexpr auto slash_reflect(::slash::reflect::Tag<Type> /*tag*/) {           \
        return ::slash::reflect::make_type<Type>(                                                  \
            #Type, is_component SLASH_DETAIL_FOR_EACH(SLASH_DETAIL_FIELD, Type, __VA_ARGS__));     \
    }

// Reflects a value type: SLASH_REFLECT(Type, field, ...). Use it at namespace scope, in the
// namespace that declares Type.
#define SLASH_REFLECT(Type, ...) SLASH_DETAIL_REFLECT(Type, false __VA_OPT__(, ) __VA_ARGS__)

// Reflects an ECS component: SLASH_COMPONENT(Type, field, ...). Same rules as SLASH_REFLECT.
#define SLASH_COMPONENT(Type, ...) SLASH_DETAIL_REFLECT(Type, true __VA_OPT__(, ) __VA_ARGS__)
