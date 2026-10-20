// The bank's rules, which src/bank.cpp and src/game.cpp both include.

#pragma once

namespace bank {

// Coins a wallet holding coins earns in interest each tick.
int interest(int coins);

} // namespace bank
