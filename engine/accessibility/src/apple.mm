// What macOS says of its screen reader. Built with ARC.

#include "internal.hpp"

#import <AppKit/AppKit.h>

namespace slash::accessibility {

bool voice_over_running() {
    return [[NSWorkspace sharedWorkspace] isVoiceOverEnabled] == YES;
}

} // namespace slash::accessibility
