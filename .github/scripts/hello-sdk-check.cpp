// Copied into the game CI exports, so each platform's export proves an SDK package links, ships
// and loads: the game calls examples/sdk-package's stand-in SDK before main, which fails to start
// if its library isn't where the platform's loader looks.

#ifdef SLASH_SDK_HELLO
#include "hello.h"

namespace {

[[maybe_unused]] const int hello_started = hello_init(HELLO_APP_ID);

} // namespace
#endif
