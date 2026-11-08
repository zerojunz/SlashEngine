#include "slash/xr/face.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The tables are Godot 4.5's, from scene/3d/xr/xr_face_modifier_3d.cpp and
// servers/xr/xr_face_tracker.h (MIT).
namespace slash::xr {

namespace {

// The face shapes, by Unified Expressions' names, in Godot's XRFaceTracker::BlendShapeEntry order.
constexpr std::array<std::string_view, kFaceShapeCount> kNames{{
    "EyeLookOutRight",       // Right eye looks outwards.
    "EyeLookInRight",        // Right eye looks inwards.
    "EyeLookUpRight",        // Right eye looks upwards.
    "EyeLookDownRight",      // Right eye looks downwards.
    "EyeLookOutLeft",        // Left eye looks outwards.
    "EyeLookInLeft",         // Left eye looks inwards.
    "EyeLookUpLeft",         // Left eye looks upwards.
    "EyeLookDownLeft",       // Left eye looks downwards.
    "EyeClosedRight",        // Closes the right eyelid.
    "EyeClosedLeft",         // Closes the left eyelid.
    "EyeSquintRight",        // Squeezes the right eye socket muscles.
    "EyeSquintLeft",         // Squeezes the left eye socket muscles.
    "EyeWideRight",          // Right eyelid widens beyond relaxed.
    "EyeWideLeft",           // Left eyelid widens beyond relaxed.
    "EyeDilationRight",      // Dilates the right eye pupil.
    "EyeDilationLeft",       // Dilates the left eye pupil.
    "EyeConstrictRight",     // Constricts the right eye pupil.
    "EyeConstrictLeft",      // Constricts the left eye pupil.
    "BrowPinchRight",        // Right eyebrow pinches in.
    "BrowPinchLeft",         // Left eyebrow pinches in.
    "BrowLowererRight",      // Outer right eyebrow pulls down.
    "BrowLowererLeft",       // Outer left eyebrow pulls down.
    "BrowInnerUpRight",      // Inner right eyebrow pulls up.
    "BrowInnerUpLeft",       // Inner left eyebrow pulls up.
    "BrowOuterUpRight",      // Outer right eyebrow pulls up.
    "BrowOuterUpLeft",       // Outer left eyebrow pulls up.
    "NoseSneerRight",        // Right side face sneers.
    "NoseSneerLeft",         // Left side face sneers.
    "NasalDilationRight",    // Right side nose canal dilates.
    "NasalDilationLeft",     // Left side nose canal dilates.
    "NasalConstrictRight",   // Right side nose canal constricts.
    "NasalConstrictLeft",    // Left side nose canal constricts.
    "CheekSquintRight",      // Raises the right side cheek.
    "CheekSquintLeft",       // Raises the left side cheek.
    "CheekPuffRight",        // Puffs the right side cheek.
    "CheekPuffLeft",         // Puffs the left side cheek.
    "CheekSuckRight",        // Sucks in the right side cheek.
    "CheekSuckLeft",         // Sucks in the left side cheek.
    "JawOpen",               // Opens jawbone.
    "MouthClosed",           // Closes the mouth.
    "JawRight",              // Pushes jawbone right.
    "JawLeft",               // Pushes jawbone left.
    "JawForward",            // Pushes jawbone forward.
    "JawBackward",           // Pushes jawbone backward.
    "JawClench",             // Flexes jaw muscles.
    "JawMandibleRaise",      // Raises the jawbone.
    "LipSuckUpperRight",     // Upper right lip part tucks in the mouth.
    "LipSuckUpperLeft",      // Upper left lip part tucks in the mouth.
    "LipSuckLowerRight",     // Lower right lip part tucks in the mouth.
    "LipSuckLowerLeft",      // Lower left lip part tucks in the mouth.
    "LipSuckCornerRight",    // Right lip corner folds into the mouth.
    "LipSuckCornerLeft",     // Left lip corner folds into the mouth.
    "LipFunnelUpperRight",   // Upper right lip part pushes into a funnel.
    "LipFunnelUpperLeft",    // Upper left lip part pushes into a funnel.
    "LipFunnelLowerRight",   // Lower right lip part pushes into a funnel.
    "LipFunnelLowerLeft",    // Lower left lip part pushes into a funnel.
    "LipPuckerUpperRight",   // Upper right lip part pushes outwards.
    "LipPuckerUpperLeft",    // Upper left lip part pushes outwards.
    "LipPuckerLowerRight",   // Lower right lip part pushes outwards.
    "LipPuckerLowerLeft",    // Lower left lip part pushes outwards.
    "MouthUpperUpRight",     // Upper right part of the lip pulls up.
    "MouthUpperUpLeft",      // Upper left part of the lip pulls up.
    "MouthLowerDownRight",   // Lower right part of the lip pulls up.
    "MouthLowerDownLeft",    // Lower left part of the lip pulls up.
    "MouthUpperDeepenRight", // Upper right lip part pushes in the cheek.
    "MouthUpperDeepenLeft",  // Upper left lip part pushes in the cheek.
    "MouthUpperRight",       // Moves upper lip right.
    "MouthUpperLeft",        // Moves upper lip left.
    "MouthLowerRight",       // Moves lower lip right.
    "MouthLowerLeft",        // Moves lower lip left.
    "MouthCornerPullRight",  // Right lip corner pulls diagonally up and out.
    "MouthCornerPullLeft",   // Left lip corner pulls diagonally up and out.
    "MouthCornerSlantRight", // Right corner lip slants up.
    "MouthCornerSlantLeft",  // Left corner lip slants up.
    "MouthFrownRight",       // Right corner lip pulls down.
    "MouthFrownLeft",        // Left corner lip pulls down.
    "MouthStretchRight",     // Mouth corner lip pulls out and down.
    "MouthStretchLeft",      // Mouth corner lip pulls out and down.
    "MouthDimpleRight",      // Right lip corner is pushed backwards.
    "MouthDimpleLeft",       // Left lip corner is pushed backwards.
    "MouthRaiserUpper",      // Raises and slightly pushes out the upper mouth.
    "MouthRaiserLower",      // Raises and slightly pushes out the lower mouth.
    "MouthPressRight",       // Right side lips press and flatten together vertically.
    "MouthPressLeft",        // Left side lips press and flatten together vertically.
    "MouthTightenerRight",   // Right side lips squeeze together horizontally.
    "MouthTightenerLeft",    // Left side lips squeeze together horizontally.
    "TongueOut",             // Tongue visibly sticks out of the mouth.
    "TongueUp",              // Tongue points upwards.
    "TongueDown",            // Tongue points downwards.
    "TongueRight",           // Tongue points right.
    "TongueLeft",            // Tongue points left.
    "TongueRoll",            // Sides of the tongue funnel, creating a roll.
    "TongueBlendDown",       // Tongue arches up then down inside the mouth.
    "TongueCurlUp",          // Tongue arches down then up inside the mouth.
    "TongueSquish",          // Tongue squishes together and thickens.
    "TongueFlat",            // Tongue flattens and thins out.
    "TongueTwistRight",      // Tongue tip rotates clockwise, with the rest following gradually.
    "TongueTwistLeft", // Tongue tip rotates counter-clockwise, with the rest following gradually.
    "SoftPalateClose", // Inner mouth throat closes.
    "ThroatSwallow",   // The Adam's apple visibly swallows.
    "NeckFlexRight",   // Right side neck visibly flexes.
    "NeckFlexLeft",    // Left side neck visibly flexes.
    "EyeClosed",       // Closes both eye lids.
    "EyeWide",         // Widens both eye lids.
    "EyeSquint",       // Squints both eye lids.
    "EyeDilation",     // Dilates both pupils.
    "EyeConstrict",    // Constricts both pupils.
    "BrowDownRight",   // Pulls the right eyebrow down and in.
    "BrowDownLeft",    // Pulls the left eyebrow down and in.
    "BrowDown",        // Pulls both eyebrows down and in.
    "BrowUpRight",     // Right brow appears worried.
    "BrowUpLeft",      // Left brow appears worried.
    "BrowUp",          // Both brows appear worried.
    "NoseSneer",       // Entire face sneers.
    "NasalDilation",   // Both nose canals dilate.
    "NasalConstrict",  // Both nose canals constrict.
    "CheekPuff",       // Puffs both cheeks.
    "CheekSuck",       // Sucks in both cheeks.
    "CheekSquint",     // Raises both cheeks.
    "LipSuckUpper",    // Tucks in the upper lips.
    "LipSuckLower",    // Tucks in the lower lips.
    "LipSuck",         // Tucks in both lips.
    "LipFunnelUpper",  // Funnels in the upper lips.
    "LipFunnelLower",  // Funnels in the lower lips.
    "LipFunnel",       // Funnels in both lips.
    "LipPuckerUpper",  // Upper lip part pushes outwards.
    "LipPuckerLower",  // Lower lip part pushes outwards.
    "LipPucker",       // Lips push outwards.
    "MouthUpperUp",    // Raises the upper lips.
    "MouthLowerDown",  // Lowers the lower lips.
    "MouthOpen",       // Mouth opens, revealing teeth.
    "MouthRight",      // Moves mouth right.
    "MouthLeft",       // Moves mouth left.
    "MouthSmileRight", // Right side of the mouth smiles.
    "MouthSmileLeft",  // Left side of the mouth smiles.
    "MouthSmile",      // Mouth expresses a smile.
    "MouthSadRight",   // Right side of the mouth expresses sadness.
    "MouthSadLeft",    // Left side of the mouth expresses sadness.
    "MouthSad",        // Mouth expresses sadness.
    "MouthStretch",    // Mouth stretches.
    "MouthDimple",     // Lip corners dimple.
    "MouthTightener",  // Mouth tightens.
    "MouthPress",      // Mouth presses together.
}};

// find_face_blend_shape's table: each face shape's names, lowercase with no underscores, in
// Unified Expressions, then ARKit, SRanipal and Meta's, where they differ.
struct NameEntry {
    std::size_t shape = 0;
    std::array<std::string_view, 4> names{};
};
constexpr std::array<NameEntry, 143> kNameTable{{
    {0, {"eyelookoutright", "eyerightright", "eyeslookoutr"}},                // EyeLookOutRight
    {1, {"eyelookinright", "eyerightleft", "eyeslookinr"}},                   // EyeLookInRight
    {2, {"eyelookupright", "eyerightlookup", "eyeslookupr"}},                 // EyeLookUpRight
    {3, {"eyelookdownright", "eyerightlookdown", "eyeslookdownr"}},           // EyeLookDownRight
    {4, {"eyelookoutleft", "eyeleftleft", "eyeslookoutl"}},                   // EyeLookOutLeft
    {5, {"eyelookinleft", "eyeleftright", "eyeslookinl"}},                    // EyeLookInLeft
    {6, {"eyelookupleft", "eyeleftlookup", "eyeslookupl"}},                   // EyeLookUpLeft
    {7, {"eyelookdownleft", "eyeleftlookdown", "eyeslookdownl"}},             // EyeLookDownLeft
    {8, {"eyeclosedright", "eyeblinkright", "eyerightblink", "eyesclosedr"}}, // EyeClosedRight
    {9, {"eyeclosedleft", "eyeblinkleft", "eyeleftblink", "eyesclosedl"}},    // EyeClosedLeft
    {10, {"eyesquintright", "eyessquintr"}},                                  // EyeSquintRight
    {11, {"eyesquintleft", "eyessquintl"}},                                   // EyeSquintLeft
    {12, {"eyewideright", "eyerightwide", "eyeswidenr"}},                     // EyeWideRight
    {13, {"eyewideleft", "eyeleftwide", "eyeswidenl"}},                       // EyeWideLeft
    {14, {"eyedilationright", "eyerightdilation"}},                           // EyeDilationRight
    {15, {"eyedilationleft", "eyeleftdilation"}},                             // EyeDilationLeft
    {16, {"eyeconstrictright", "eyerightconstrict"}},                         // EyeConstrictRight
    {17, {"eyeconstrictleft", "eyeleftconstrict"}},                           // EyeConstrictLeft
    {18, {"browpinchright"}},                                                 // BrowPinchRight
    {19, {"browpinchleft"}},                                                  // BrowPinchLeft
    {20, {"browlowererright"}},                                               // BrowLowererRight
    {21, {"browlowererleft"}},                                                // BrowLowererLeft
    {22, {"browinnerupright", "innerbrowraiserr"}},                           // BrowInnerUpRight
    {23, {"browinnerupleft", "innerbrowraiserl"}},                            // BrowInnerUpLeft
    {24, {"browouterupright", "outerbrowraiserr"}},                           // BrowOuterUpRight
    {25, {"browouterupleft", "outerbrowraiserl"}},                            // BrowOuterUpLeft
    {26, {"nosesneerright", "nosewrinklerr"}},                                // NoseSneerRight
    {27, {"nosesneerleft", "nosewrinklerl"}},                                 // NoseSneerLeft
    {28, {"nasaldilationright"}},                                             // NasalDilationRight
    {29, {"nasaldilationleft"}},                                              // NasalDilationLeft
    {30, {"nasalconstrictright"}},                                            // NasalConstrictRight
    {31, {"nasalconstrictleft"}},                                             // NasalConstrictLeft
    {32, {"cheeksquintright", "cheekraiserr"}},                               // CheekSquintRight
    {33, {"cheeksquintleft", "cheekraiserl"}},                                // CheekSquintLeft
    {34, {"cheekpuffright", "cheekpuffr"}},                                   // CheekPuffRight
    {35, {"cheekpuffleft", "cheekpuffl"}},                                    // CheekPuffLeft
    {36, {"cheeksuckright", "cheeksuckr"}},                                   // CheekSuckRight
    {37, {"cheeksuckleft", "cheeksuckl"}},                                    // CheekSuckLeft
    {38, {"jawopen", "jawdrop"}},                                             // JawOpen
    {39, {"mouthclosed", "mouthclose", "mouthapeshape", "lipstoward"}},       // MouthClosed
    {40, {"jawright", "jawsidewaysright"}},                                   // JawRight
    {41, {"jawleft", "jawsidewaysleft"}},                                     // JawLeft
    {42, {"jawforward", "jawthrust"}},                                        // JawForward
    {43, {"jawbackward"}},                                                    // JawBackward
    {44, {"jawclench"}},                                                      // JawClench
    {45, {"jawmandibleraise"}},                                               // JawMandibleRaise
    {46, {"lipsuckupperright", "lipsuckrt"}},                                 // LipSuckUpperRight
    {47, {"lipsuckupperleft", "lipsucklt"}},                                  // LipSuckUpperLeft
    {48, {"lipsucklowerright", "lipsuckrb"}},                                 // LipSuckLowerRight
    {49, {"lipsucklowerleft", "lipsucklb"}},                                  // LipSuckLowerLeft
    {50, {"lipsuckcornerright"}},                                             // LipSuckCornerRight
    {51, {"lipsuckcornerleft"}},                                              // LipSuckCornerLeft
    {52, {"lipfunnelupperright", "lipfunnelerrt"}},                           // LipFunnelUpperRight
    {53, {"lipfunnelupperleft", "lipfunnelerlt"}},                            // LipFunnelUpperLeft
    {54, {"lipfunnellowerright", "lipsuckrb"}},                               // LipFunnelLowerRight
    {55, {"lipfunnellowerleft", "lipsucklb"}},                                // LipFunnelLowerLeft
    {56, {"lippuckerupperright"}},                                            // LipPuckerUpperRight
    {57, {"lippuckerupperleft"}},                                             // LipPuckerUpperLeft
    {58, {"lippuckerlowerright"}},                                            // LipPuckerLowerRight
    {59, {"lippuckerlowerleft"}},                                             // LipPuckerLowerLeft
    {60, {"mouthupperupright", "upperlipraiserr"}},                           // MouthUpperUpRight
    {61, {"mouthupperupleft", "upperlipraiserl"}},                            // MouthUpperUpLeft
    {62, {"mouthlowerdownright", "mouthlowerupright", "lowerlipdepressorr"}}, // MouthLowerDownRight
    {63, {"mouthlowerdownleft", "mouthlowerupleft", "lowerlipdepressorl"}},   // MouthLowerDownLeft
    {64, {"mouthupperdeepenright"}},                              // MouthUpperDeepenRight
    {65, {"mouthupperdeepenleft"}},                               // MouthUpperDeepenLeft
    {66, {"mouthupperright"}},                                    // MouthUpperRight
    {67, {"mouthupperleft"}},                                     // MouthUpperLeft
    {68, {"mouthlowerright"}},                                    // MouthLowerRight
    {69, {"mouthlowerleft"}},                                     // MouthLowerLeft
    {70, {"mouthcornerpullright"}},                               // MouthCornerPullRight
    {71, {"mouthcornerpullleft"}},                                // MouthCornerPullLeft
    {72, {"mouthcornerslantright"}},                              // MouthCornerSlantRight
    {73, {"mouthcornerslantleft"}},                               // MouthCornerSlantLeft
    {74, {"mouthfrownright", "lipcornerdepressorr"}},             // MouthFrownRight
    {75, {"mouthfrownleft", "lipcornerdepressorl"}},              // MouthFrownLeft
    {76, {"mouthstretchright", "lipstretcherr"}},                 // MouthStretchRight
    {77, {"mouthstretchleft", "lipstretcherl"}},                  // MouthStretchLeft
    {78, {"mouthdimplerright", "mouthdimpleright", "dimplerr"}},  // MouthDimpleRight
    {79, {"mouthdimplerleft", "mouthdimpleleft", "dimplerl"}},    // MouthDimpleLeft
    {80, {"mouthraiserupper", "mouthshrugupper", "chinraisert"}}, // MouthRaiserUpper
    {81,
     {"mouthraiserlower", "mouthshruglower", "mouthloweroverlay",
      "chinraiserb"}},                                             // MouthRaiserLower
    {82, {"mouthpressright", "lippressorr"}},                      // MouthPressRight
    {83, {"mouthpressleft", "lippressorl"}},                       // MouthPressLeft
    {84, {"mouthtightenerright", "liptightenerr"}},                // MouthTightenerRight
    {85, {"mouthtightenerleft", "liptightenerl"}},                 // MouthTightenerLeft
    {86, {"tongueout", "tonguelongstep2"}},                        // TongueOut
    {87, {"tongueup"}},                                            // TongueUp
    {88, {"tonguedown"}},                                          // TongueDown
    {89, {"tongueright"}},                                         // TongueRight
    {90, {"tongueleft"}},                                          // TongueLeft
    {91, {"tongueroll"}},                                          // TongueRoll
    {92, {"tongueblenddown"}},                                     // TongueBlendDown
    {93, {"tonguecurlup"}},                                        // TongueCurlUp
    {94, {"tonguesquish"}},                                        // TongueSquish
    {95, {"tongueflat"}},                                          // TongueFlat
    {96, {"tonguetwistright"}},                                    // TongueTwistRight
    {97, {"tonguetwistleft"}},                                     // TongueTwistLeft
    {98, {"softpalateclose"}},                                     // SoftPalateClose
    {99, {"throatswallow"}},                                       // ThroatSwallow
    {100, {"neckflexright"}},                                      // NeckFlexRight
    {101, {"neckflexleft"}},                                       // NeckFlexLeft
    {102, {"eyeclosed"}},                                          // EyeClosed
    {103, {"eyewide"}},                                            // EyeWide
    {104, {"eyesquint"}},                                          // EyeSquint
    {105, {"eyedilation"}},                                        // EyeDilation
    {106, {"eyeconstrict"}},                                       // EyeConstrict
    {107, {"browdownright", "browlowererr"}},                      // BrowDownRight
    {108, {"browdownleft", "browlowererl"}},                       // BrowDownLeft
    {109, {"browdown"}},                                           // BrowDown
    {110, {"browupright"}},                                        // BrowUpRight
    {111, {"browupleft"}},                                         // BrowUpLeft
    {112, {"browup"}},                                             // BrowUp
    {113, {"nosesneer"}},                                          // NoseSneer
    {114, {"nasaldilation"}},                                      // NasalDilation
    {115, {"nasalconstrict"}},                                     // NasalConstrict
    {116, {"cheekpuff"}},                                          // CheekPuff
    {117, {"cheeksuck"}},                                          // CheekSuck
    {118, {"cheeksquint"}},                                        // CheekSquint
    {119, {"lipsuckupper", "mouthrollupper", "mouthupperinside"}}, // LipSuckUpper
    {120, {"lipsucklower", "mouthrolllower", "mouthlowerinside"}}, // LipSuckLower
    {121, {"lipsuck"}},                                            // LipSuck
    {122, {"lipfunnelupper", "mouthupperoverturn"}},               // LipFunnelUpper
    {123, {"lipfunnellower", "mouthloweroverturn"}},               // LipFunnelLower
    {124, {"lipfunnel", "mouthfunnel"}},                           // LipFunnel
    {125, {"lippuckerupper"}},                                     // LipPuckerUpper
    {126, {"lippuckerlower"}},                                     // LipPuckerLower
    {127, {"lippucker", "mouthpucker", "mouthpout"}},              // LipPucker
    {128, {"mouthupperup"}},                                       // MouthUpperUp
    {129, {"mouthlowerdown"}},                                     // MouthLowerDown
    {130, {"mouthopen"}},                                          // MouthOpen
    {131, {"mouthright"}},                                         // MouthRight
    {132, {"mouthleft"}},                                          // MouthLeft
    {133, {"mouthsmileright", "lipcornerpullerr"}},                // MouthSmileRight
    {134, {"mouthsmileleft", "lipcornerpullerl"}},                 // MouthSmileLeft
    {135, {"mouthsmile"}},                                         // MouthSmile
    {136, {"mouthsadright"}},                                      // MouthSadRight
    {137, {"mouthsadleft"}},                                       // MouthSadLeft
    {138, {"mouthsad"}},                                           // MouthSad
    {139, {"mouthstretch"}},                                       // MouthStretch
    {140, {"mouthdimple"}},                                        // MouthDimple
    {141, {"mouthtightener"}},                                     // MouthTightener
    {142, {"mouthpress"}},                                         // MouthPress
}};

// remove_driven_unified_blend_shapes' table: each blended shape and the shapes it is made of.
struct BlendedEntry {
    std::size_t shape = 0;
    std::array<int, 4> parts{};
};
constexpr std::array<BlendedEntry, 41> kBlended{{
    {102, {8, 9, -1, -1}},   // EyeClosed
    {103, {12, 13, -1, -1}}, // EyeWide
    {104, {10, 11, -1, -1}}, // EyeSquint
    {105, {14, 15, -1, -1}}, // EyeDilation
    {106, {16, 17, -1, -1}}, // EyeConstrict
    {107, {20, 18, -1, -1}}, // BrowDownRight
    {108, {21, 19, -1, -1}}, // BrowDownLeft
    {109, {20, 18, 21, 19}}, // BrowDown
    {110, {22, 24, -1, -1}}, // BrowUpRight
    {111, {23, 25, -1, -1}}, // BrowUpLeft
    {112, {22, 24, 23, 25}}, // BrowUp
    {113, {26, 27, -1, -1}}, // NoseSneer
    {114, {28, 29, -1, -1}}, // NasalDilation
    {115, {30, 31, -1, -1}}, // NasalConstrict
    {116, {34, 35, -1, -1}}, // CheekPuff
    {117, {36, 37, -1, -1}}, // CheekSuck
    {118, {32, 33, -1, -1}}, // CheekSquint
    {119, {46, 47, -1, -1}}, // LipSuckUpper
    {120, {48, 49, -1, -1}}, // LipSuckLower
    {121, {46, 47, 48, 49}}, // LipSuck
    {122, {52, 53, -1, -1}}, // LipFunnelUpper
    {123, {54, 55, -1, -1}}, // LipFunnelLower
    {124, {52, 53, 54, 55}}, // LipFunnel
    {125, {56, 57, -1, -1}}, // LipPuckerUpper
    {126, {58, 59, -1, -1}}, // LipPuckerLower
    {127, {56, 57, 58, 59}}, // LipPucker
    {128, {60, 61, -1, -1}}, // MouthUpperUp
    {129, {62, 63, -1, -1}}, // MouthLowerDown
    {130, {60, 61, 62, 63}}, // MouthOpen
    {131, {66, 68, -1, -1}}, // MouthRight
    {132, {67, 69, -1, -1}}, // MouthLeft
    {133, {70, 72, -1, -1}}, // MouthSmileRight
    {134, {71, 73, -1, -1}}, // MouthSmileLeft
    {135, {70, 72, 71, 73}}, // MouthSmile
    {136, {74, 76, -1, -1}}, // MouthSadRight
    {137, {75, 77, -1, -1}}, // MouthSadLeft
    {138, {74, 76, 75, 77}}, // MouthSad
    {139, {76, 77, -1, -1}}, // MouthStretch
    {140, {78, 79, -1, -1}}, // MouthDimple
    {141, {84, 85, -1, -1}}, // MouthTightener
    {142, {82, 83, -1, -1}}, // MouthPress
}};

// Each blended shape's parts, as indices, from kBlended.
struct Parts {
    std::array<std::size_t, 4> shapes{};
    std::size_t count = 0;
};

const std::array<Parts, kFaceShapeCount>& parts_table() {
    static const std::array<Parts, kFaceShapeCount> kParts = [] {
        std::array<Parts, kFaceShapeCount> out{};
        for (const BlendedEntry& entry : kBlended) {
            Parts& parts = out.at(entry.shape);
            for (const int part : entry.parts) {
                if (part >= 0) {
                    parts.shapes.at(parts.count++) = static_cast<std::size_t>(part);
                }
            }
        }
        return out;
    }();
    return kParts;
}

} // namespace

std::string_view face_shape_name(std::size_t shape) {
    return shape < kNames.size() ? kNames.at(shape) : std::string_view{};
}

std::optional<std::size_t> face_shape_named(std::string_view name) {
    // Godot: the name in lower case, without underscores.
    std::string key;
    key.reserve(name.size());
    for (const char c : name) {
        if (c != '_') {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    for (const NameEntry& entry : kNameTable) {
        for (const std::string_view candidate : entry.names) {
            if (candidate.empty()) {
                break;
            }
            if (key == candidate) {
                return entry.shape;
            }
        }
    }
    return std::nullopt;
}

std::span<const std::size_t> face_shape_parts(std::size_t shape) {
    if (shape >= kFaceShapeCount) {
        return {};
    }
    const Parts& parts = parts_table().at(shape);
    return std::span<const std::size_t>(parts.shapes).first(parts.count);
}

std::map<std::string, std::size_t> face_blend_mapping(std::span<const std::string> names) {
    // identify_face_blend_shapes: by face shape, the last name found for it.
    std::map<std::size_t, std::string> found;
    for (const std::string& name : names) {
        if (const std::optional<std::size_t> shape = face_shape_named(name)) {
            found[*shape] = name;
        }
    }
    // remove_driven_unified_blend_shapes
    for (const BlendedEntry& entry : kBlended) {
        const bool all = std::ranges::all_of(entry.parts, [&](int part) {
            return part < 0 || found.contains(static_cast<std::size_t>(part));
        });
        if (all) {
            found.erase(entry.shape);
        }
    }
    std::map<std::string, std::size_t> out;
    for (auto& [shape, name] : found) {
        out.emplace(std::move(name), shape);
    }
    return out;
}

void fill_blended_shapes(std::span<float, kFaceShapeCount> weights,
                         std::span<bool, kFaceShapeCount> given) {
    // Parts a given blended shape implies: the mean of the given blended shapes made of each.
    std::array<float, kFaceShapeCount> sums{};
    std::array<float, kFaceShapeCount> counts{};
    for (const BlendedEntry& entry : kBlended) {
        if (!given[entry.shape]) {
            continue;
        }
        for (const int part : entry.parts) {
            if (part >= 0 && !given[static_cast<std::size_t>(part)]) {
                sums.at(static_cast<std::size_t>(part)) += weights[entry.shape];
                counts.at(static_cast<std::size_t>(part)) += 1.0F;
            }
        }
    }
    for (std::size_t shape = 0; shape < kFaceShapeCount; ++shape) {
        if (counts.at(shape) > 0.0F) {
            weights[shape] = sums.at(shape) / counts.at(shape);
            given[shape] = true;
        }
    }
    // Then each blended shape left out whose parts are all known: their mean.
    for (const BlendedEntry& entry : kBlended) {
        if (given[entry.shape]) {
            continue;
        }
        float sum = 0.0F;
        float count = 0.0F;
        bool all = true;
        for (const int part : entry.parts) {
            if (part >= 0) {
                all = all && given[static_cast<std::size_t>(part)];
                sum += weights[static_cast<std::size_t>(part)];
                count += 1.0F;
            }
        }
        if (all) {
            weights[entry.shape] = sum / count;
            given[entry.shape] = true;
        }
    }
}

} // namespace slash::xr
