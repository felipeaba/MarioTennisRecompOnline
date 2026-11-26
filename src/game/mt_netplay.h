#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mt::netplay {

enum class Role { Offline, Host, Guest, Invalid };

inline constexpr uint32_t input_delay = 2;

struct Pad {
    uint16_t buttons = 0;
    int8_t x = 0;
    int8_t y = 0;
};

using Pads = std::array<Pad, 4>;

unsigned player_count();
unsigned pads_per_peer();
unsigned first_local_port();

struct StateCheck {
    uint32_t step = 0;
    uint32_t hash = 0;
};

Role role();

// Complete the UDP and game compatibility handshakes before starting the game.
// After startup, only the guest simulation thread may access the session.
bool start(std::string_view compatibility_manifest);

// One call per simulation tick, beginning at 0. The local pad is scheduled
// input_delay ticks ahead. Reuse the admitted pads on repeated guest reads.
bool admit(uint32_t tick, Pads local, StateCheck state, Pads& pads);
void drain_replay();

} // namespace mt::netplay
