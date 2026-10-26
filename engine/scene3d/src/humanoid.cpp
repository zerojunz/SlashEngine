#include "slash/scene3d/humanoid.hpp"

#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/skin_tool.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Godot's SkeletonProfileHumanoid and BoneMapper::auto_mapping_process. Godot matches bone
// names with PCRE2 regular expressions; the few constructs its patterns use (alternatives,
// groups, ".*", character classes and negative lookarounds) are matched here by a small
// backtracking matcher, so the patterns read as Godot's.
namespace slash::scene3d {
namespace {

// SkeletonProfileHumanoid's bones, as its constructor sets them.
// NOLINTBEGIN(modernize-use-std-numbers): Godot's numbers, such as 0.577, as it writes them.
constexpr std::array<ProfileBone, 56> kHumanoid{{
    {"Root",
     "",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.0F, 0.0F},
     "Body",
     false},
    {"Hips",
     "Root",
     TailDirection::SpecificChild,
     "Spine",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.75F, 0.0F},
     "Body",
     true},
    {"Spine",
     "Hips",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.1F, 0.0F},
     "Body",
     true},
    {"Chest",
     "Spine",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.1F, 0.0F},
     "Body",
     false},
    {"UpperChest",
     "Chest",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.1F, 0.0F},
     "Body",
     false},
    {"Neck",
     "UpperChest",
     TailDirection::SpecificChild,
     "Head",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.1F, 0.0F},
     "Body",
     false},
    {"Head",
     "Neck",
     TailDirection::End,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.1F, 0.0F},
     "Body",
     true},
    {"LeftEye",
     "Head",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F},
     {0.05F, 0.15F, 0.0F},
     "Face",
     false},
    {"RightEye",
     "Head",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F},
     {-0.05F, 0.15F, 0.0F},
     "Face",
     false},
    {"Jaw",
     "Head",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F},
     {0.0F, 0.05F, 0.05F},
     "Face",
     false},
    {"LeftShoulder",
     "UpperChest",
     TailDirection::AverageChildren,
     "",
     {0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F},
     {0.05F, 0.1F, 0.0F},
     "Body",
     true},
    {"LeftUpperArm",
     "LeftShoulder",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.05F, 0.0F},
     "Body",
     true},
    {"LeftLowerArm",
     "LeftUpperArm",
     TailDirection::AverageChildren,
     "",
     {0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F},
     {0.0F, 0.25F, 0.0F},
     "Body",
     true},
    {"LeftHand",
     "LeftLowerArm",
     TailDirection::SpecificChild,
     "LeftMiddleProximal",
     {0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 0.0F},
     {0.0F, 0.25F, 0.0F},
     "Body",
     true},
    {"LeftThumbMetacarpal",
     "LeftHand",
     TailDirection::AverageChildren,
     "",
     {0.0F, -0.577F, 0.816F, 0.0F, 0.816F, 0.577F, -1.0F, 0.0F, 0.0F},
     {-0.025F, 0.025F, 0.0F},
     "LeftHand",
     false},
    {"LeftThumbProximal",
     "LeftThumbMetacarpal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.043F, 0.0F},
     "LeftHand",
     false},
    {"LeftThumbDistal",
     "LeftThumbProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.043F, 0.0F},
     "LeftHand",
     false},
    {"LeftIndexProximal",
     "LeftHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {-0.025F, 0.075F, 0.0F},
     "LeftHand",
     false},
    {"LeftIndexIntermediate",
     "LeftIndexProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "LeftHand",
     false},
    {"LeftIndexDistal",
     "LeftIndexIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "LeftHand",
     false},
    {"LeftMiddleProximal",
     "LeftHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.075F, 0.0F},
     "LeftHand",
     false},
    {"LeftMiddleIntermediate",
     "LeftMiddleProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.075F, 0.0F},
     "LeftHand",
     false},
    {"LeftMiddleDistal",
     "LeftMiddleIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "LeftHand",
     false},
    {"LeftRingProximal",
     "LeftHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.025F, 0.075F, 0.0F},
     "LeftHand",
     false},
    {"LeftRingIntermediate",
     "LeftRingProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "LeftHand",
     false},
    {"LeftRingDistal",
     "LeftRingIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "LeftHand",
     false},
    {"LeftLittleProximal",
     "LeftHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.05F, 0.05F, 0.0F},
     "LeftHand",
     false},
    {"LeftLittleIntermediate",
     "LeftLittleProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "LeftHand",
     false},
    {"LeftLittleDistal",
     "LeftLittleIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "LeftHand",
     false},
    {"RightShoulder",
     "UpperChest",
     TailDirection::AverageChildren,
     "",
     {0.0F, -1.0F, 0.0F, 0.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F},
     {-0.05F, 0.1F, 0.0F},
     "Body",
     true},
    {"RightUpperArm",
     "RightShoulder",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.05F, 0.0F},
     "Body",
     true},
    {"RightLowerArm",
     "RightUpperArm",
     TailDirection::AverageChildren,
     "",
     {0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 0.0F},
     {0.0F, 0.25F, 0.0F},
     "Body",
     true},
    {"RightHand",
     "RightLowerArm",
     TailDirection::SpecificChild,
     "RightMiddleProximal",
     {0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F},
     {0.0F, 0.25F, 0.0F},
     "Body",
     true},
    {"RightThumbMetacarpal",
     "RightHand",
     TailDirection::AverageChildren,
     "",
     {0.0F, 0.577F, -0.816F, 0.0F, 0.816F, 0.577F, 1.0F, 0.0F, 0.0F},
     {0.025F, 0.025F, 0.0F},
     "RightHand",
     false},
    {"RightThumbProximal",
     "RightThumbMetacarpal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.043F, 0.0F},
     "RightHand",
     false},
    {"RightThumbDistal",
     "RightThumbProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.043F, 0.0F},
     "RightHand",
     false},
    {"RightIndexProximal",
     "RightHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.025F, 0.075F, 0.0F},
     "RightHand",
     false},
    {"RightIndexIntermediate",
     "RightIndexProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "RightHand",
     false},
    {"RightIndexDistal",
     "RightIndexIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "RightHand",
     false},
    {"RightMiddleProximal",
     "RightHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.075F, 0.0F},
     "RightHand",
     false},
    {"RightMiddleIntermediate",
     "RightMiddleProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.075F, 0.0F},
     "RightHand",
     false},
    {"RightMiddleDistal",
     "RightMiddleIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "RightHand",
     false},
    {"RightRingProximal",
     "RightHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {-0.025F, 0.075F, 0.0F},
     "RightHand",
     false},
    {"RightRingIntermediate",
     "RightRingProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "RightHand",
     false},
    {"RightRingDistal",
     "RightRingIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "RightHand",
     false},
    {"RightLittleProximal",
     "RightHand",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {-0.05F, 0.05F, 0.0F},
     "RightHand",
     false},
    {"RightLittleIntermediate",
     "RightLittleProximal",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.05F, 0.0F},
     "RightHand",
     false},
    {"RightLittleDistal",
     "RightLittleIntermediate",
     TailDirection::AverageChildren,
     "",
     {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.0F, 0.025F, 0.0F},
     "RightHand",
     false},
    {"LeftUpperLeg",
     "Hips",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {0.1F, 0.0F, 0.0F},
     "Body",
     true},
    {"LeftLowerLeg",
     "LeftUpperLeg",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.375F, 0.0F},
     "Body",
     true},
    {"LeftFoot",
     "LeftLowerLeg",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, -1.0F, 0.0F},
     {0.0F, 0.375F, 0.0F},
     "Body",
     true},
    {"LeftToes",
     "LeftFoot",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.15F, 0.0F},
     "Body",
     false},
    {"RightUpperLeg",
     "Hips",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.0F, 1.0F},
     {-0.1F, 0.0F, 0.0F},
     "Body",
     true},
    {"RightLowerLeg",
     "RightUpperLeg",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.375F, 0.0F},
     "Body",
     true},
    {"RightFoot",
     "RightLowerLeg",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, -1.0F, 0.0F},
     {0.0F, 0.375F, 0.0F},
     "Body",
     true},
    {"RightToes",
     "RightFoot",
     TailDirection::AverageChildren,
     "",
     {-1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, -1.0F},
     {0.0F, 0.15F, 0.0F},
     "Body",
     false},
}};
// NOLINTEND(modernize-use-std-numbers)

// ---- Godot's RegEx, for the patterns auto mapping uses ----

class Pattern {
public:
    explicit Pattern(std::string_view text) {
        std::size_t at = 0;
        alternatives_ = parse(text, at);
    }

    // RegEx::search(subject).is_valid(): whether it matches anywhere in subject.
    [[nodiscard]] bool search(std::string_view subject) const {
        for (std::size_t start = 0; start <= subject.size(); ++start) {
            if (match_any(alternatives_, subject, start, [](std::size_t) { return true; })) {
                return true;
            }
        }
        return false;
    }

private:
    struct Atom;
    using Sequence = std::vector<Atom>;
    using Next = std::function<bool(std::size_t)>;
    enum class Kind : std::uint8_t { Literal, Any, Class, Group, NotAhead, NotBehind };
    struct Atom {
        Kind kind = Kind::Literal;
        char literal = 0;
        std::vector<std::pair<char, char>> ranges; // a class's, inclusive
        std::vector<Sequence> alternatives;        // a group's or a lookaround's
        bool star = false;
    };

    static std::vector<Sequence> parse(std::string_view text, std::size_t& at) {
        std::vector<Sequence> out(1);
        while (at < text.size() && text[at] != ')') {
            const char c = text[at];
            if (c == '|') {
                out.emplace_back();
                ++at;
                continue;
            }
            Atom atom;
            if (c == '(') {
                ++at;
                if (text.substr(at, 2) == "?!") {
                    atom.kind = Kind::NotAhead;
                    at += 2;
                } else if (text.substr(at, 3) == "?<!") {
                    atom.kind = Kind::NotBehind;
                    at += 3;
                } else {
                    atom.kind = Kind::Group;
                }
                atom.alternatives = parse(text, at);
                ++at; // ')'
            } else if (c == '[') {
                atom.kind = Kind::Class;
                ++at;
                while (at < text.size() && text[at] != ']') {
                    const char low = text[at];
                    char high = low;
                    if (at + 2 < text.size() && text[at + 1] == '-' && text[at + 2] != ']') {
                        high = text[at + 2];
                        at += 2;
                    }
                    atom.ranges.emplace_back(low, high);
                    ++at;
                }
                ++at; // ']'
            } else {
                atom.kind = c == '.' ? Kind::Any : Kind::Literal;
                atom.literal = c;
                ++at;
            }
            if (at < text.size() && text[at] == '*') {
                atom.star = true;
                ++at;
            }
            out.back().push_back(std::move(atom));
        }
        return out;
    }

    // Whether a one-character atom matches c.
    static bool single(const Atom& atom, char c) {
        switch (atom.kind) {
        case Kind::Any:
            return c != '\n';
        case Kind::Class:
            return std::ranges::any_of(atom.ranges, [c](const auto& range) {
                return c >= range.first && c <= range.second;
            });
        case Kind::Literal:
            return c == atom.literal;
        case Kind::Group:
        case Kind::NotAhead:
        case Kind::NotBehind:
            break;
        }
        return false;
    }

    static bool match_any(const std::vector<Sequence>& alternatives, std::string_view s,
                          std::size_t i, const Next& next) {
        return std::ranges::any_of(
            alternatives, [&](const Sequence& sequence) { return match(sequence, 0, s, i, next); });
    }

    static bool match(const Sequence& sequence, std::size_t k, std::string_view s, std::size_t i,
                      const Next& next) {
        if (k == sequence.size()) {
            return next(i);
        }
        const Atom& atom = sequence[k];
        const Next rest = [&](std::size_t j) { return match(sequence, k + 1, s, j, next); };
        if (atom.star) {
            // Greedy: as many as match, then fewer.
            std::size_t end = i;
            while (end < s.size() && single(atom, s[end])) {
                ++end;
            }
            for (std::size_t j = end + 1; j-- > i;) {
                if (rest(j)) {
                    return true;
                }
            }
            return false;
        }
        switch (atom.kind) {
        case Kind::Group:
            return match_any(atom.alternatives, s, i, rest);
        case Kind::NotAhead:
            return !match_any(atom.alternatives, s, i, [](std::size_t) { return true; }) && rest(i);
        case Kind::NotBehind: {
            // Godot's patterns look one character behind.
            const bool behind =
                i > 0 && std::ranges::any_of(atom.alternatives, [&](const Sequence& alternative) {
                    return alternative.size() == 1 && single(alternative[0], s[i - 1]);
                });
            return !behind && rest(i);
        }
        case Kind::Any:
        case Kind::Class:
        case Kind::Literal:
            break;
        }
        return i < s.size() && single(atom, s[i]) && rest(i + 1);
    }

    std::vector<Sequence> alternatives_;
};

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Godot's String::to_snake_case (_separate_compound_words, then spaces as underscores), for ASCII.
std::string to_snake_case(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string words;
    std::size_t start = 0;
    bool prev_upper = upper(text[0]);
    bool prev_lower = lower(text[0]);
    bool prev_digit = digit(text[0]);
    for (std::size_t i = 1; i < text.size(); ++i) {
        const bool curr_upper = upper(text[i]);
        const bool curr_lower = lower(text[i]);
        const bool curr_digit = digit(text[i]);
        const bool next_lower = i + 1 < text.size() && lower(text[i + 1]);
        const bool cond_a = prev_lower && curr_upper;                               // aA
        const bool cond_b = (prev_upper || prev_digit) && curr_upper && next_lower; // AAa, 2Aa
        const bool cond_c = prev_digit && curr_lower && next_lower;                 // 2aa
        const bool cond_d = (prev_upper || prev_lower) && curr_digit;               // A2, a2
        if (cond_a || cond_b || cond_c || cond_d) {
            words += text.substr(start, i - start);
            words += ' ';
            start = i;
        }
        prev_upper = curr_upper;
        prev_lower = curr_lower;
        prev_digit = curr_digit;
    }
    words += text.substr(start);
    for (char& c : words) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '_' || c == '-') {
            c = '_';
        }
    }
    return to_lower(words);
}

// A skeleton as BoneMapper sees it: its bones in Godot's order, by name, with their parents,
// children and rests in the skeleton's space.
class Skeleton {
public:
    Skeleton(const Model& model, std::vector<std::size_t> bones)
        : model_(model), nodes_(std::move(bones)), parents_(nodes_.size(), -1),
          children_(nodes_.size()), global_rest_(nodes_.size()) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const int parent = model_.nodes[nodes_[i]].parent;
            for (std::size_t j = 0; j < nodes_.size() && parent >= 0; ++j) {
                if (nodes_[j] == static_cast<std::size_t>(parent)) {
                    parents_[i] = static_cast<int>(j);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (parents_[i] >= 0) {
                children_[static_cast<std::size_t>(parents_[i])].push_back(static_cast<int>(i));
            }
        }
        const std::vector<NodePose> rest = rest_pose(model_);
        // Parents before children: Godot's bones may come after their children only across
        // skeleton roots, which have no parent here.
        std::vector<bool> done(nodes_.size(), false);
        const std::function<void(std::size_t)> place = [&](std::size_t i) {
            if (done[i]) {
                return;
            }
            const world::Affine3D local = node_transform(rest[nodes_[i]]);
            if (parents_[i] >= 0) {
                place(static_cast<std::size_t>(parents_[i]));
                global_rest_[i] = global_rest_[static_cast<std::size_t>(parents_[i])] * local;
            } else {
                global_rest_[i] = local;
            }
            done[i] = true;
        };
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            place(i);
        }
    }

    [[nodiscard]] int size() const { return static_cast<int>(nodes_.size()); }
    [[nodiscard]] const std::string& name(int bone) const {
        static const std::string kNone;
        return bone >= 0 && bone < size()
                   ? model_.nodes[nodes_[static_cast<std::size_t>(bone)]].name
                   : kNone;
    }
    [[nodiscard]] int find(std::string_view name) const {
        for (int i = 0; i < size(); ++i) {
            if (this->name(i) == name) {
                return i;
            }
        }
        return -1;
    }
    [[nodiscard]] int parent(int bone) const {
        return bone >= 0 && bone < size() ? parents_[static_cast<std::size_t>(bone)] : -1;
    }
    [[nodiscard]] std::vector<int> children(int bone) const {
        return bone >= 0 && bone < size() ? children_[static_cast<std::size_t>(bone)]
                                          : std::vector<int>{};
    }
    [[nodiscard]] std::vector<int> parentless() const {
        std::vector<int> out;
        for (int i = 0; i < size(); ++i) {
            if (parents_[static_cast<std::size_t>(i)] < 0) {
                out.push_back(i);
            }
        }
        return out;
    }
    [[nodiscard]] Vec3 global_origin(int bone) const {
        return global_rest_[static_cast<std::size_t>(bone)].origin;
    }

private:
    const Model& model_;
    std::vector<std::size_t> nodes_;
    std::vector<int> parents_;
    std::vector<std::vector<int>> children_;
    std::vector<world::Affine3D> global_rest_;
};

enum class Segregation : std::uint8_t { None, Left, Right };

// BoneMapper::guess_bone_segregation.
Segregation guess_bone_segregation(std::string_view bone_name) {
    static const std::array<Pattern, 2> kLeft{Pattern("(?<![a-zA-Z])left"),
                                              Pattern("(?<![a-zA-Z0-9])l(?![a-zA-Z0-9])")};
    static const std::array<Pattern, 2> kRight{Pattern("(?<![a-zA-Z])right"),
                                               Pattern("(?<![a-zA-Z0-9])r(?![a-zA-Z0-9])")};
    const std::string fixed = to_snake_case(bone_name);
    for (std::size_t i = 0; i < kLeft.size(); ++i) {
        if (kLeft.at(i).search(fixed)) {
            return Segregation::Left;
        }
        if (kRight.at(i).search(fixed)) {
            return Segregation::Right;
        }
    }
    return Segregation::None;
}

// BoneMapper::is_match_with_bone_name.
bool is_match_with_bone_name(std::string_view bone_name, std::string_view word) {
    return Pattern(word).search(to_lower(bone_name));
}

// BoneMapper::search_bone_by_name: the bone a word of the pick list names, the earlier words
// first, among the parent's descendants (or every bone), or the bones between the child and the
// parent; of several, the one with the shortest name, which is probably the original.
int search_bone_by_name(const Skeleton& skeleton, const std::vector<std::string_view>& picklist,
                        Segregation segregation = Segregation::None, int parent = -1,
                        int child = -1, int children_count = -1) {
    std::vector<std::string> hits;
    std::string shortest;
    for (const std::string_view word : picklist) {
        if (child == -1) {
            std::vector<int> queue =
                parent == -1 ? skeleton.parentless() : skeleton.children(parent);
            while (!queue.empty()) {
                const int idx = queue.front();
                queue.erase(queue.begin());
                const std::vector<int> children = skeleton.children(idx);
                queue.insert(queue.end(), children.begin(), children.end());
                if (children_count == 0 && !children.empty()) {
                    continue;
                }
                if (children_count > 0 && static_cast<int>(children.size()) < children_count) {
                    continue;
                }
                const std::string& bn = skeleton.name(idx);
                if (is_match_with_bone_name(bn, word) &&
                    guess_bone_segregation(bn) == segregation) {
                    hits.push_back(bn);
                }
            }
            if (!hits.empty()) {
                shortest = hits[0];
                for (const std::string& hit : hits) {
                    if (hit.size() < shortest.size()) {
                        shortest = hit; // Prioritize parent.
                    }
                }
            }
        } else {
            int idx = skeleton.parent(child);
            while (idx != parent && idx >= 0) {
                const std::vector<int> children = skeleton.children(idx);
                const bool counted =
                    (children_count != 0 || children.empty()) &&
                    (children_count <= 0 || static_cast<int>(children.size()) >= children_count);
                const std::string& bn = skeleton.name(idx);
                if (counted && is_match_with_bone_name(bn, word) &&
                    guess_bone_segregation(bn) == segregation) {
                    hits.push_back(bn);
                }
                idx = skeleton.parent(idx);
            }
            if (!hits.empty()) {
                shortest = hits[0];
                for (const std::string& hit : hits) {
                    if (hit.size() <= shortest.size()) {
                        shortest = hit; // Prioritize parent.
                    }
                }
            }
        }
        if (!shortest.empty()) {
            break;
        }
    }
    return shortest.empty() ? -1 : skeleton.find(shortest);
}

bool is_zero_approx(Vec3 v) {
    constexpr float kEpsilon = 0.00001F; // Godot's CMP_EPSILON
    return std::abs(v.x) < kEpsilon && std::abs(v.y) < kEpsilon && std::abs(v.z) < kEpsilon;
}

// The fingers of one hand, as auto_mapping_process's step 4-1 maps them.
void map_fingers(const Skeleton& skeleton, int hand_or_palm, Segregation side,
                 std::string_view prefix, BoneMap& map) {
    constexpr std::array<std::string_view, 5> kFingers{"thumb|pollex", "index|fore", "middle",
                                                       "ring", "little|pinkie|pinky"};
    constexpr std::array<std::array<std::string_view, 3>, 5> kNames{{
        {"ThumbMetacarpal", "ThumbProximal", "ThumbDistal"},
        {"IndexProximal", "IndexIntermediate", "IndexDistal"},
        {"MiddleProximal", "MiddleIntermediate", "MiddleDistal"},
        {"RingProximal", "RingIntermediate", "RingDistal"},
        {"LittleProximal", "LittleIntermediate", "LittleDistal"},
    }};
    const auto set = [&](std::size_t finger, std::size_t joint, int bone) {
        map[std::string(prefix) + std::string(kNames.at(finger).at(joint))] = skeleton.name(bone);
    };
    // The bones of one finger from its root, as many as it has up to three, past any carpal.
    const auto assign = [&](std::size_t finger, std::vector<int> path) {
        if (path.size() > 3) {
            path.erase(path.begin(), path.end() - 3); // Eliminate the possibility of carpal bone.
        }
        for (std::size_t j = 0; j < path.size(); ++j) {
            set(finger, j, path[j]);
        }
        return !path.empty();
    };
    static const Pattern kDistal("distal");
    static const Pattern kTip("tip|leaf");
    int tips_index = -1;
    bool thumb_tips_size = false;
    bool named_finger_is_found = false;
    for (std::size_t i = 0; i < kFingers.size(); ++i) {
        std::vector<int> search_path;
        int finger = search_bone_by_name(skeleton, {kFingers.at(i)}, side, hand_or_palm, -1, 0);
        if (finger == -1) {
            continue;
        }
        while (finger != hand_or_palm && finger >= 0) {
            search_path.push_back(finger);
            finger = skeleton.parent(finger);
        }
        // Tips detection by name matching with "distal" from root.
        for (int j = static_cast<int>(search_path.size()) - 1; j >= 0; --j) {
            if (kDistal.search(to_lower(skeleton.name(search_path[static_cast<std::size_t>(j)])))) {
                tips_index = j - 1;
                break;
            }
        }
        // Tips detection by name matching with "tip|leaf" from end.
        if (tips_index < 0) {
            for (std::size_t j = 0; j < search_path.size(); ++j) {
                if (kTip.search(to_lower(skeleton.name(search_path[j])))) {
                    tips_index = static_cast<int>(j);
                    break;
                }
            }
        }
        // Tips detection by thumb children size.
        if (tips_index < 0) {
            if (i == 0) {
                thumb_tips_size = search_path.size() > 3;
            }
            tips_index = (thumb_tips_size ? 1 : 0) - 1;
        }
        // Remove tips.
        for (int j = 0; j <= tips_index && !search_path.empty(); ++j) {
            search_path.erase(search_path.begin());
        }
        std::ranges::reverse(search_path);
        named_finger_is_found = assign(i, std::move(search_path)) || named_finger_is_found;
    }

    // It is a bit corner case, but possibly the finger names are sequentially numbered...
    if (!named_finger_is_found) {
        static const Pattern kFinger("finger");
        std::vector<std::string> finger_names;
        for (const int child : skeleton.children(hand_or_palm)) {
            if (kFinger.search(to_lower(skeleton.name(child)))) {
                finger_names.push_back(skeleton.name(child));
            }
        }
        std::ranges::sort(finger_names); // Normal use cases never have more than 10 fingers.
        for (std::size_t i = 0; i < finger_names.size() && i < kFingers.size(); ++i) {
            std::vector<int> search_path;
            const int finger_root = skeleton.find(finger_names[i]);
            int finger = search_bone_by_name(skeleton, {"finger"}, side, finger_root, -1, 0);
            if (finger != -1) {
                while (finger != finger_root && finger >= 0) {
                    search_path.push_back(finger);
                    finger = skeleton.parent(finger);
                }
            }
            search_path.push_back(finger_root);
            // Tips detection by thumb children size.
            if (i == 0) {
                thumb_tips_size = search_path.size() > 3;
            }
            tips_index = (thumb_tips_size ? 1 : 0) - 1;
            for (int j = 0; j <= tips_index && !search_path.empty(); ++j) {
                search_path.erase(search_path.begin());
            }
            std::ranges::reverse(search_path);
            assign(i, std::move(search_path));
        }
    }
}

// A hand, or a palm with five children, then the wrist above it when there is one
// (auto_mapping_process's step 4).
std::pair<int, int> find_hand(const Skeleton& skeleton, Segregation side, int hips) {
    const std::vector<std::string_view> picklist{"hand", "wrist", "palm", "fingers"};
    int hand_or_palm = search_bone_by_name(skeleton, picklist, side, hips, -1, 5);
    if (hand_or_palm == -1) {
        // Ambiguous, but try again for fewer finger models.
        hand_or_palm = search_bone_by_name(skeleton, picklist, side, hips);
    }
    int hand = hand_or_palm; // Check for the presence of a wrist, since bones with five
                             // children may be palmar.
    while (hand != -1) {
        const int above = search_bone_by_name(skeleton, picklist, side, hips, hand);
        if (above == -1) {
            break;
        }
        hand = above;
    }
    return {hand_or_palm, hand};
}

// BoneMapper::auto_mapping_process.
BoneMap auto_mapping_process(const Skeleton& skeleton) {
    BoneMap map;
    const auto set = [&](std::string_view profile_bone, int bone) {
        if (bone >= 0) {
            map[std::string(profile_bone)] = skeleton.name(bone);
        }
    };

    // 1. Guess Hips
    const int hips = search_bone_by_name(skeleton, {"hip", "pelvis", "waist", "torso", "spine"});
    if (hips == -1) {
        return map; // If there is no Hips, we cannot guess bone after then.
    }
    set("Hips", hips);

    // 2. Guess Root
    std::vector<int> search_path;
    for (int bone = skeleton.parent(hips); bone >= 0; bone = skeleton.parent(bone)) {
        search_path.push_back(bone);
    }
    int bone_idx = -1;
    if (search_path.size() == 1) {
        bone_idx = search_path[0]; // It is only one bone which can be root.
    } else if (!search_path.empty()) {
        static const Pattern kRoot("root");
        for (const int candidate : search_path) {
            if (kRoot.search(to_lower(skeleton.name(candidate)))) {
                bone_idx = candidate; // Name match is preferred.
                break;
            }
        }
        if (bone_idx == -1) {
            for (const int candidate : search_path) {
                if (is_zero_approx(skeleton.global_origin(candidate))) {
                    bone_idx = candidate; // The bone at the origin is appropriate as a root.
                    break;
                }
            }
        }
        if (bone_idx == -1) {
            bone_idx = search_path.back(); // Ambiguous, but most parental bone selected.
        }
    }
    set("Root", bone_idx);

    // 3. Guess Foots
    const std::vector<std::string_view> feet{"foot", "ankle"};
    const int left_foot = search_bone_by_name(skeleton, feet, Segregation::Left, hips);
    set("LeftFoot", left_foot);
    const int right_foot = search_bone_by_name(skeleton, feet, Segregation::Right, hips);
    set("RightFoot", right_foot);

    // 3-1. Guess LowerLegs
    const std::vector<std::string_view> lower_legs{"(low|under).*leg", "knee", "shin", "calf",
                                                   "leg"};
    const int left_lower_leg =
        left_foot != -1
            ? search_bone_by_name(skeleton, lower_legs, Segregation::Left, hips, left_foot)
            : -1;
    set("LeftLowerLeg", left_lower_leg);
    const int right_lower_leg =
        right_foot != -1
            ? search_bone_by_name(skeleton, lower_legs, Segregation::Right, hips, right_foot)
            : -1;
    set("RightLowerLeg", right_lower_leg);

    // 3-2. Guess UpperLegs
    const std::vector<std::string_view> upper_legs{"up.*leg", "thigh", "leg"};
    if (left_lower_leg != -1) {
        set("LeftUpperLeg",
            search_bone_by_name(skeleton, upper_legs, Segregation::Left, hips, left_lower_leg));
    }
    if (right_lower_leg != -1) {
        set("RightUpperLeg",
            search_bone_by_name(skeleton, upper_legs, Segregation::Right, hips, right_lower_leg));
    }

    // 3-3. Guess Toes
    const auto toes = [&](int foot, Segregation side) {
        if (foot == -1) {
            return -1;
        }
        int toe = search_bone_by_name(skeleton, {"toe", "ball"}, side, foot);
        if (toe == -1) {
            const std::vector<int> children = skeleton.children(foot);
            if (children.size() == 1) {
                toe = children[0]; // Maybe only one child of the Foot is Toes.
            }
        }
        return toe;
    };
    set("LeftToes", toes(left_foot, Segregation::Left));
    set("RightToes", toes(right_foot, Segregation::Right));

    // 4. Guess Hands
    const auto [left_hand_or_palm, left_hand] = find_hand(skeleton, Segregation::Left, hips);
    set("LeftHand", left_hand);
    const auto [right_hand_or_palm, right_hand] = find_hand(skeleton, Segregation::Right, hips);
    set("RightHand", right_hand);

    // 4-1. Guess Finger
    if (left_hand_or_palm != -1) {
        map_fingers(skeleton, left_hand_or_palm, Segregation::Left, "Left", map);
    }
    if (right_hand_or_palm != -1) {
        map_fingers(skeleton, right_hand_or_palm, Segregation::Right, "Right", map);
    }

    // 5. Guess Arms
    const std::vector<std::string_view> shoulders{"shoulder", "clavicle", "collar"};
    const int left_shoulder = search_bone_by_name(skeleton, shoulders, Segregation::Left, hips);
    set("LeftShoulder", left_shoulder);
    const int right_shoulder = search_bone_by_name(skeleton, shoulders, Segregation::Right, hips);
    set("RightShoulder", right_shoulder);

    // 5-1. Guess LowerArms
    const std::vector<std::string_view> lower_arms{"(low|fore).*arm", "elbow", "arm"};
    const int left_lower_arm = left_shoulder != -1 && left_hand_or_palm != -1
                                   ? search_bone_by_name(skeleton, lower_arms, Segregation::Left,
                                                         left_shoulder, left_hand_or_palm)
                                   : -1;
    set("LeftLowerArm", left_lower_arm);
    const int right_lower_arm = right_shoulder != -1 && right_hand_or_palm != -1
                                    ? search_bone_by_name(skeleton, lower_arms, Segregation::Right,
                                                          right_shoulder, right_hand_or_palm)
                                    : -1;
    set("RightLowerArm", right_lower_arm);

    // 5-2. Guess UpperArms
    const std::vector<std::string_view> upper_arms{"up.*arm", "arm"};
    if (left_shoulder != -1 && left_lower_arm != -1) {
        set("LeftUpperArm", search_bone_by_name(skeleton, upper_arms, Segregation::Left,
                                                left_shoulder, left_lower_arm));
    }
    if (right_shoulder != -1 && right_lower_arm != -1) {
        set("RightUpperArm", search_bone_by_name(skeleton, upper_arms, Segregation::Right,
                                                 right_shoulder, right_lower_arm));
    }

    // 6. Guess Neck
    int neck = search_bone_by_name(skeleton, {"neck", "head", "face"}, Segregation::None, hips);
    if (neck == -1) {
        // If it can't expect by name, search child spine of where the right and left shoulders
        // (or hands) cross.
        int ls_idx = left_shoulder != -1 ? left_shoulder : left_hand_or_palm;
        int rs_idx = right_shoulder != -1 ? right_shoulder : right_hand_or_palm;
        if (ls_idx != -1 && rs_idx != -1) {
            bool detect = false;
            while (ls_idx != hips && ls_idx >= 0 && rs_idx != hips && rs_idx >= 0) {
                ls_idx = skeleton.parent(ls_idx);
                rs_idx = skeleton.parent(rs_idx);
                if (ls_idx == rs_idx) {
                    detect = true;
                    break;
                }
            }
            if (detect) {
                for (const int child : skeleton.children(ls_idx)) {
                    // Limited to "spine", which could otherwise be breast, wing and so on.
                    if (child != ls_idx && child != rs_idx &&
                        is_match_with_bone_name(skeleton.name(child), "spine")) {
                        neck = child;
                        break;
                    }
                }
            }
        }
    }

    // 7. Guess Head
    int head = search_bone_by_name(skeleton, {"head", "face"}, Segregation::None, neck);
    if (head == -1 && neck != -1) {
        const std::vector<int> children = skeleton.children(neck);
        if (children.size() == 1) {
            head = children[0]; // Maybe only one child of the Neck is Head.
        }
    }
    if (head == -1) {
        if (neck != -1) {
            head = neck; // The head animation should have more movement.
            neck = -1;
            set("Head", head);
        }
    } else {
        set("Neck", neck);
        set("Head", head);
    }

    const int neck_or_head = neck != -1 ? neck : head;
    if (neck_or_head != -1) {
        // 7-1. Guess Eyes
        const std::vector<std::string_view> eyes{"eye(?!.*(brow|lash|lid))"};
        set("LeftEye", search_bone_by_name(skeleton, eyes, Segregation::Left, neck_or_head));
        set("RightEye", search_bone_by_name(skeleton, eyes, Segregation::Right, neck_or_head));
        // 7-2. Guess Jaw
        set("Jaw", search_bone_by_name(skeleton, {"jaw"}, Segregation::None, neck_or_head));
    }

    // 8. Guess UpperChest or Chest
    if (neck_or_head == -1) {
        return map; // Abort.
    }
    int chest_or_upper_chest = skeleton.parent(neck_or_head);
    bool is_appropriate = true;
    for (const int shoulder : {left_shoulder, right_shoulder}) {
        if (shoulder == -1) {
            continue;
        }
        bool detect = false;
        for (int bone = skeleton.parent(shoulder); bone != hips && bone >= 0;
             bone = skeleton.parent(bone)) {
            if (bone == chest_or_upper_chest) {
                detect = true;
                break;
            }
        }
        is_appropriate = is_appropriate && detect;
    }
    if (!is_appropriate) {
        chest_or_upper_chest = skeleton.parent(left_shoulder) == skeleton.parent(right_shoulder)
                                   ? skeleton.parent(left_shoulder)
                                   : -1;
    }
    if (chest_or_upper_chest == -1) {
        return map; // Will be not able to guess Spines.
    }

    // 9. Guess Spines
    search_path.clear();
    for (int bone = skeleton.parent(chest_or_upper_chest); bone != hips && bone >= 0;
         bone = skeleton.parent(bone)) {
        search_path.push_back(bone);
    }
    std::ranges::reverse(search_path);
    if (search_path.empty()) {
        set("Spine", chest_or_upper_chest); // Maybe chibi model...?
    } else if (search_path.size() == 1) {
        set("Spine", search_path[0]);
        set("Chest", chest_or_upper_chest);
    } else {
        set("Spine", search_path[0]);
        set("Chest", search_path.back()); // Probably UpperChest's parent is appropriate.
        set("UpperChest", chest_or_upper_chest);
    }
    return map;
}

} // namespace

std::span<const ProfileBone> humanoid_profile() {
    return kHumanoid;
}

int find_profile_bone(std::string_view name) {
    for (std::size_t i = 0; i < kHumanoid.size(); ++i) {
        if (kHumanoid.at(i).name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

BoneMap auto_map_bones(const Model& model) {
    const std::vector<ModelSkeleton> skeletons = model_skeletons(model);
    const ModelSkeleton* largest = nullptr;
    for (const ModelSkeleton& skeleton : skeletons) {
        if (largest == nullptr || skeleton.bones.size() > largest->bones.size()) {
            largest = &skeleton;
        }
    }
    if (largest == nullptr) {
        return {};
    }
    return auto_mapping_process(Skeleton(model, largest->bones));
}

} // namespace slash::scene3d
