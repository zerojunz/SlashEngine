// The bank's rules. main.cpp edits src/bank.hpp, so build.check compiles this file again too.

#include "bank.hpp"

namespace bank {

int interest(int coins) {
    return coins / 100;
}

} // namespace bank
