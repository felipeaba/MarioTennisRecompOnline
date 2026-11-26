#include "mt_netplay.h"

#include <cstdio>
#include <cstdlib>

#if defined(MT_ENABLE_NETPLAY)
#include <bit>
#include <charconv>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <algorithm>

#include "recomp_net/session.h"
#endif

namespace mt::netplay {

Role role() {
    const bool host = std::getenv("MT_NETPLAY_HOST") != nullptr;
    const bool join = std::getenv("MT_NETPLAY_JOIN") != nullptr;
    if (host && join) return Role::Invalid;
    if (host) return Role::Host;
    if (join) return Role::Guest;
    return Role::Offline;
}

unsigned player_count() {
    const char* value = std::getenv("MT_NETPLAY_MODE");
    return value != nullptr && (std::string_view(value) == "2+2" ||
                                std::string_view(value) == "1+1+1+1") ? 4 : 2;
}

unsigned pads_per_peer() {
    const char* value = std::getenv("MT_NETPLAY_MODE");
    return value != nullptr && std::string_view(value) == "2+2" ? 2 : 1;
}

unsigned first_local_port() {
    if (role() != Role::Guest) return 0;
    const char* slot = std::getenv("MT_NETPLAY_SEAT");
    if (player_count() == 4 && pads_per_peer() == 1 && slot && slot[0] >= '2' &&
            slot[0] <= '4' && slot[1] == '\0')
        return unsigned(slot[0] - '1');
    return pads_per_peer();
}

#if defined(MT_ENABLE_NETPLAY)
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto connect_timeout = 25s;
constexpr auto doubles_connect_timeout = 60s;
constexpr auto input_timeout = 8s;
constexpr auto doubles_startup_timeout = 25s;
constexpr auto manifest_retry = 150ms;
constexpr size_t manifest_capacity = 512;
constexpr unsigned pad_bytes = 4;
constexpr rnet_u8 accept_manifest = 0;
constexpr rnet_u8 incompatible_options = 3;
constexpr rnet_u8 seat_occupied = 4;

bool valid_number(std::string_view text, unsigned minimum, unsigned maximum) {
    unsigned value = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end && value >= minimum && value <= maximum;
}

bool valid_peer(std::string_view endpoint) {
    const size_t colon = endpoint.find(':');
    if (colon == std::string_view::npos || !valid_number(endpoint.substr(colon + 1), 1, 65535)) return false;

    auto address = endpoint.substr(0, colon);
    for (int i = 0; i < 3; ++i) {
        const size_t dot = address.find('.');
        if (dot == std::string_view::npos || !valid_number(address.substr(0, dot), 0, 255)) return false;
        address.remove_prefix(dot + 1);
    }
    return valid_number(address, 0, 255);
}

void encode_u32(rnet_u8* output, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        output[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t decode_u32(const rnet_u8* input) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t{input[i]} << (8 * i);
    }
    return value;
}

struct Session {
    RNetSession* handle = nullptr;
    std::string manifest, announcement;
    unsigned local_slot = 0;
    Clock::time_point next_manifest{};
    unsigned matches = 0;
    unsigned accepted = 0;
    unsigned remote_mask = 0;
    unsigned slot_count = 2;
    unsigned per_peer = 1;
    bool ready() const { return matches == remote_mask && accepted == remote_mask; }
    uint32_t next_tick = 0;
    Pads local{};
    StateCheck local_state{};
    Pads resolved{};
    std::array<StateCheck, 4> compared{};
    uint32_t last_matching_step = 0;
    bool valid_pads = false;

    ~Session() {
        if (handle != nullptr) {
            rnet_session_send_bye(handle);
            rnet_session_destroy(handle);
        }
    }

    static void sample_local(rnet_u32, RNetInputSample* out, void* context) {
        const auto& session = *static_cast<Session*>(context);
        const unsigned state_offset = pad_bytes * session.per_peer;
        out->size = state_offset + 8;
        for (unsigned i = 0; i < session.per_peer; ++i) {
            const Pad& pad = session.local[i];
            auto* payload = out->bytes + i * pad_bytes;
            payload[0] = static_cast<uint8_t>(pad.buttons >> 8);
            payload[1] = static_cast<uint8_t>(pad.buttons);
            payload[2] = std::bit_cast<uint8_t>(pad.x);
            payload[3] = std::bit_cast<uint8_t>(pad.y);
        }
        encode_u32(out->bytes + state_offset, session.local_state.step);
        encode_u32(out->bytes + state_offset + 4, session.local_state.hash);
        out->valid = 1;
    }

    static void publish(rnet_u32 tick, const RNetInputSample* rows, int slots, void* context) {
        auto& session = *static_cast<Session*>(context);
        session.valid_pads = false;
        if (slots != static_cast<int>(session.slot_count)) return;

        const unsigned state_offset = pad_bytes * session.per_peer;
        for (int slot = 0; slot < slots; ++slot) {
            const auto& row = rows[slot];
            if (!row.valid) return;
            const bool neutral = tick < input_delay && row.size == 0;
            if (!neutral && row.size != state_offset + 8) return;
            session.compared[slot] = neutral ? StateCheck{} :
                StateCheck{decode_u32(row.bytes + state_offset), decode_u32(row.bytes + state_offset + 4)};
            for (unsigned i = 0; i < session.per_peer; ++i) {
                const auto* payload = row.bytes + i * pad_bytes;
                session.resolved[slot * session.per_peer + i] = neutral ? Pad{} : Pad{
                    static_cast<uint16_t>((uint16_t{payload[0]} << 8) | payload[1]),
                    std::bit_cast<int8_t>(payload[2]), std::bit_cast<int8_t>(payload[3])
                };
            }
        }
        session.valid_pads = true;
    }

    bool pump() {
        rnet_session_pump(handle);
        if (rnet_session_peer_wire_version_mismatch(handle) != 0) {
            std::fprintf(stderr, "[netplay] incompatible recomp-net wire version\n");
            return false;
        }
        rnet_u8 status = 0;
        while (rnet_session_take_modset_ack(handle, &status, nullptr, 0)) {
            const int seat = rnet_session_rb_last_take_from(handle);
            if (seat < 0 || seat >= static_cast<int>(slot_count) || !(remote_mask & (1u << seat)))
                continue;
            if (status == seat_occupied && seat == 0 && slot_count == 4) {
                std::fprintf(stderr, "[netplay] seat already occupied, requested player %u\n", local_slot + 1);
                return false;
            }
            if (status != accept_manifest) {
                std::fprintf(stderr, "[netplay] peer rejected game compatibility (%u)\n", static_cast<unsigned>(status));
                return false;
            }
            accepted |= 1u << seat;
        }
        if (!rnet_session_is_running(handle)) return true;

        const auto now = Clock::now();
        // Keep announcing until the other seats have had time to boot.
        if ((!ready() || next_tick < 120) && now >= next_manifest) {
            if (rnet_session_send_modset(handle, announcement.c_str()) != 0) {
                std::fprintf(stderr, "[netplay] could not send compatibility manifest\n");
                return false;
            }
            next_manifest = now + manifest_retry + std::chrono::milliseconds(17 * local_slot);
        }

        // Reply to retransmits during gameplay too. The peer may have lost our
        // last ACK while we were starting the game.
        char remote[manifest_capacity];
        if (rnet_session_take_modset(handle, remote, sizeof(remote))) {
            const bool has_seat = slot_count == 4 && remote[0] >= '0' && remote[0] <= '3' && remote[1] == ':';
            const unsigned seat = has_seat ? unsigned(remote[0] - '0') : 0;
            if ((slot_count == 4 && (!has_seat || !(remote_mask & (1u << seat)))) ||
                manifest != (has_seat ? remote + 2 : remote)) {
                rnet_session_send_modset_ack(handle, incompatible_options, "game compatibility manifest differs");
                std::fprintf(stderr, "[netplay] peer version, ROM, save profile or gameplay settings differ\n");
                return false;
            }
            if (rnet_session_send_modset_ack(handle, accept_manifest, "") != 0) {
                std::fprintf(stderr, "[netplay] could not acknowledge compatibility\n");
                return false;
            }
            matches |= slot_count == 4 ? 1u << seat : remote_mask;
        }

        if (rnet_session_peer_disconnected(handle, 0)) {
            std::fprintf(stderr, "[netplay] peer left the session\n");
            return false;
        }
        return true;
    }
};

std::unique_ptr<Session> active_session;
} // namespace
#endif

bool start(std::string_view compatibility_manifest) {
    const Role mode = role();
    if (mode == Role::Offline) return true;
    if (mode == Role::Invalid) {
        std::fprintf(stderr, "[netplay] choose either MT_NETPLAY_HOST or MT_NETPLAY_JOIN\n");
        return false;
    }

#if defined(MT_ENABLE_NETPLAY)
    if (compatibility_manifest.empty() || compatibility_manifest.size() >= manifest_capacity ||
            compatibility_manifest.find('\0') != std::string_view::npos) {
        std::fprintf(stderr, "[netplay] invalid compatibility manifest\n");
        return false;
    }

    const bool hosting = mode == Role::Host;
    const char* endpoint = std::getenv(hosting ? "MT_NETPLAY_HOST" : "MT_NETPLAY_JOIN");
    const std::string_view address(endpoint);
    if (hosting ? !valid_number(address, 1, 65535) : !valid_peer(address)) {
        std::fprintf(stderr, "[netplay] use MT_NETPLAY_HOST=port or MT_NETPLAY_JOIN=IPv4:port\n");
        return false;
    }

    const char* setting = std::getenv("MT_NETPLAY_MODE");
    const std::string_view layout = setting ? setting : "1+1";
    if (layout != "1+1" && layout != "2+2" && layout != "1+1+1+1") {
        std::fprintf(stderr, "[netplay] invalid player layout\n");
        return false;
    }
    const bool hub = layout == "1+1+1+1";
    const char* seat_name = std::getenv("MT_NETPLAY_SEAT");
    if (hub && !hosting && (!seat_name || std::string_view(seat_name).size() != 1 || !valid_number(seat_name, 2, 4))) {
        std::fprintf(stderr, "[netplay] select guest seat 2, 3 or 4\n");
        return false;
    }
    auto session = std::make_unique<Session>();
    session->manifest = compatibility_manifest;
    RNetConfig config{};
    rnet_config_init_defaults(&config);
    config.slot_count = hub ? 4 : 2;
    config.local_slot = static_cast<rnet_u8>(first_local_port() / pads_per_peer());
    session->local_slot = config.local_slot;
    session->slot_count = config.slot_count;
    session->per_peer = pads_per_peer();
    session->remote_mask = ((1u << config.slot_count) - 1u) & ~(1u << config.local_slot);
    if (layout != "1+1") session->manifest += ";layout=" + std::string(layout);
    session->announcement = hub ? std::to_string(config.local_slot) + ":" + session->manifest : session->manifest;
    config.input_delay = input_delay;
    config.protocol_magic = layout == "1+1" ? 0x4D544E32u : hub ? 0x4D543448u : 0x4D543432u;

    RNetHostVTable callbacks{};
    callbacks.sample_local = Session::sample_local;
    callbacks.publish = Session::publish;
    callbacks.ctx = session.get();
    session->handle = rnet_session_create(&config, &callbacks);
    const std::string bind = hosting ? "0.0.0.0:" + std::string(address) : "0.0.0.0:0";
    if (session->handle == nullptr ||
            (hub && hosting ? rnet_session_start_lan_hub(session->handle, bind.c_str()) :
              rnet_session_start_lan(session->handle, bind.c_str(), hosting ? nullptr : endpoint)) != 0) {
        std::fprintf(stderr, "[netplay] could not open UDP session (%s)\n", endpoint);
        return false;
    }

    std::fprintf(stderr, "[netplay] %s %s, waiting for compatibility handshake\n", hosting ? "hosting" : "joining", endpoint);
    const auto deadline = Clock::now() + (player_count() == 4 ? doubles_connect_timeout : connect_timeout);
    unsigned reported_connected = 0;
    do {
        if (!session->pump()) return false;
        if (player_count() == 4) {
            unsigned connected = 1;
            for (unsigned seat = 0; seat < session->slot_count; ++seat) {
                if (seat != session->local_slot &&
                    rnet_session_peer_rx_age_ms(session->handle, static_cast<int>(seat)) <= 3000 &&
                    !rnet_session_peer_gone(session->handle, static_cast<int>(seat)))
                    ++connected;
            }
            if (connected != reported_connected) {
                reported_connected = connected;
                std::fprintf(stderr, "[netplay] lobby occupancy %u/%u\n", connected, session->slot_count);
            }
        }
        if (session->ready()) {
            std::fprintf(stderr, "[netplay] compatibility verified, local player %u, delay %u frames\n",
                         first_local_port() + 1u, input_delay);
            active_session = std::move(session);
            return true;
        }
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);

    std::fprintf(stderr, "[netplay] compatibility handshake timed out\n");
#else
    (void)compatibility_manifest;
    std::fprintf(stderr, "[netplay] this executable was built without recomp-net\n");
#endif
    return false;
}

bool admit(uint32_t tick, Pads local, StateCheck state, Pads& pads) {
#if defined(MT_ENABLE_NETPLAY)
    if (active_session == nullptr || tick != active_session->next_tick) {
        std::fprintf(stderr, "[netplay] unexpected simulation tick %u\n", tick);
        return false;
    }
    auto& session = *active_session;
    // Entering the next tick is the caller's confirmation that the previous
    // guest step completed. Do not advance for repeated controller polls.
    if (tick != 0) rnet_session_advance(session.handle);
    session.local = local;
    session.local_state = state;

    const auto timeout = tick == 0 && player_count() == 4 ? doubles_startup_timeout : input_timeout;
    const auto deadline = Clock::now() + timeout;
    do {
        if (!session.pump()) return false;
        if (rnet_session_try_admit(session.handle, tick)) {
            if (!session.valid_pads) {
                std::fprintf(stderr, "[netplay] invalid controller packet at tick %u\n", tick);
                return false;
            }
            const auto host = session.compared[0];
            if (host.step != 0) {
                bool all_match = true;
                for (unsigned slot = 1; slot < session.slot_count; ++slot) {
                    const auto peer = session.compared[slot];
                    if (peer.step != host.step) {
                        all_match = false;
                    } else if (peer.hash != host.hash) {
                        std::fprintf(stderr, "[netplay] court desync step=%u peer=%u host=%08X remote=%08X\n",
                                     host.step, slot + 1, host.hash, peer.hash);
                        return false;
                    }
                }
                if (all_match && host.step > session.last_matching_step) {
                    session.last_matching_step = host.step;
                    std::fprintf(stderr, "[netplay] court checkpoint step=%u hash=%08X\n", host.step, host.hash);
                }
            }
            pads = session.resolved;
            ++session.next_tick;
            if (tick % 300 == 0) {
                RNetSessionStats stats{};
                rnet_session_get_stats(session.handle, &stats);
                std::fprintf(stderr, "[netplay] tick=%u rx=%u input_bundles=%u wait_ms=%u max_wait_ms=%u\n",
                             tick, stats.packets_rx, stats.input_bundle_sends, stats.last_admit_wait_ms, stats.max_admit_wait_ms);
            }
            return true;
        }

        rnet_u32 desync_tick, local_hash, remote_hash;
        if (rnet_session_input_desync(session.handle, &desync_tick, &local_hash, &remote_hash)) {
            std::fprintf(stderr, "[netplay] input desync tick=%u local=%08X remote=%08X\n", desync_tick, local_hash, remote_hash);
            return false;
        }
        // Readable sockets can stay hot with retransmissions. Pace retries
        // explicitly so both peers cannot amplify each other's traffic.
        std::this_thread::sleep_for(2ms);
    } while (Clock::now() < deadline);

    std::fprintf(stderr, "[netplay] timed out waiting for input at tick %u\n", tick);
#else
    (void)tick;
    (void)local;
    (void)state;
    (void)pads;
#endif
    return false;
}

void drain_replay() {
#if defined(MT_ENABLE_NETPLAY)
    // Keep sending the final acknowledgements while the other endpoint drains
    // its last admission. The guest no longer advances during this period.
    auto& session = *active_session;
    const auto deadline = Clock::now() + 750ms;
    do {
        rnet_session_pump(session.handle);
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    active_session.reset();
#endif
}

} // namespace mt::netplay
