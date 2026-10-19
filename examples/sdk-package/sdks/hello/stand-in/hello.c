#include "hello.h"

static int started = 0;

int hello_init(const char* app_id) {
    started = app_id != 0 && app_id[0] != '\0';
    return started;
}

const char* hello_greeting(void) {
    return started ? "Hello from the SDK" : "The SDK has not started";
}
