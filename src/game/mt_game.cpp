// Mario Tennis (USA) specific runtime glue: boot, overlay registration and the loader hook.
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "mt_game.h"
#ifdef MT_ENABLE_NETPLAY
#include "mt_netplay.h"
#endif
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#ifndef MT_LIVE_RECOMP
#include "../../RecompiledFuncs/recomp_overlays.inl"
#endif

namespace {
    struct OverlayInfo {
        uint32_t real_rom;   // offset of the LZ-compressed segment in the original ROM
        uint32_t vrom;       // virtual ROM offset of the recompiled section
        uint32_t vram;       // address the segment is linked for
        uint32_t size;       // decompressed size
    };

    constexpr OverlayInfo overlay_table[] = {
#include "mt_overlay_table.inc"
    };

    struct Loaded { uint32_t ram, size; };
    std::vector<Loaded> loaded;  // invariant: never overlapping

    // Fully unload anything the range overwrites, then register the section there.
    void activate_section(uint32_t vrom, uint32_t ram, uint32_t size) {
        for (auto it = loaded.begin(); it != loaded.end();) {
            if (it->ram < ram + size && ram < it->ram + it->size) {
                unload_overlays((int32_t)it->ram, it->size);
                it = loaded.erase(it);
            }
            else {
                ++it;
            }
        }
        load_overlays(vrom, (int32_t)ram, size);
        loaded.push_back({ ram, size });
    }

    constexpr uint32_t boot_vram        = 0x80300000;
    constexpr uint32_t boot_lz_offset   = 0x350;       // compressed main inside the boot copy
    constexpr uint32_t main_vram        = 0x80031000;
    constexpr uint32_t main_lz_size     = 0x2AE10;     // from the LZ header; checked below

    // Same algorithm as the boot loader at 0x80300080 (see tools/mt_lz.py).
    uint32_t lz_decompress(uint8_t* rdram, uint32_t src, uint32_t dst) {
        const uint32_t dst_start = dst;
        src += 4;
        while (true) {
            uint8_t flags = MEM_BU(0, (gpr)(int32_t)src++);
            if (flags == 0) {
                for (int i = 0; i < 8; i++) MEM_B(0, (gpr)(int32_t)dst++) = MEM_BU(0, (gpr)(int32_t)src++);
                continue;
            }
            for (int bit = 0; bit < 8; bit++) {
                if (flags & (0x80 >> bit)) {
                    uint8_t b0 = MEM_BU(0, (gpr)(int32_t)src);
                    uint8_t b1 = MEM_BU(1, (gpr)(int32_t)src);
                    uint32_t dist = ((b0 & 0xF0u) << 4) | b1;
                    if (dist == 0) return dst - dst_start;
                    uint32_t n = b0 & 0xF, len;
                    if (n == 0) { len = MEM_BU(2, (gpr)(int32_t)src) + 0x11u; src += 3; }
                    else { len = (n == 1) ? 2 : n + 1; src += 2; }
                    uint32_t from = dst - dist;
                    for (uint32_t k = 0; k < len; k++) MEM_B(0, (gpr)(int32_t)(dst + k)) = MEM_BU(0, (gpr)(int32_t)(from + k));
                    dst += len;
                }
                else {
                    MEM_B(0, (gpr)(int32_t)dst++) = MEM_BU(0, (gpr)(int32_t)src++);
                }
            }
        }
    }

    void clear_ram(uint8_t* rdram, uint32_t start, uint32_t end) {
        for (uint32_t a = start; a < end; a += 4) MEM_W(0, (gpr)(int32_t)a) = 0;
    }
}

#ifndef MT_LIVE_RECOMP // live builds: src/game/mt_live.cpp
void mt::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}
#endif

void mt::on_init(uint8_t* rdram, recomp_context* ctx) {
    // IPL3 copies ROM 0x1000..+1 MiB to the header entrypoint (0x80300000).
    recomp::do_rom_read(rdram, (gpr)(int32_t)boot_vram, 0x10001000, 0x100000);

    // Boot stub: clear its .bss, clear 0x80000400-0x80100000, decompress main. The runtime's own
    // 1 MiB copy to 0x80031000 is cleared as well (on hardware that range is not written).
    clear_ram(rdram, 0x803106C0, 0x803106D0);
    clear_ram(rdram, 0x80000400, boot_vram);
    uint32_t out = lz_decompress(rdram, boot_vram + boot_lz_offset, main_vram);
    if (out != main_lz_size) {
        fprintf(stderr, "[mt] main decompressed to 0x%X bytes, expected 0x%X\n", out, main_lz_size);
    }
    loaded.clear();
    fprintf(stderr, "[mt] boot: main decompressed (0x%X bytes), entering 0x%08X\n", out, main_vram);

    // Registers as the stub leaves them when it jumps to main.
    ctx->r29 = (gpr)(int32_t)0x803FF000;  // sp
    ctx->r4 = 0x116C0;                    // ROM offset of the segment table
    ctx->r5 = 0;                          // COP0 Count at boot start
    ctx->r6 = 0;                          // COP0 Count after decompression
}

// ROM offset passed to the loader, captured at its entry (the loader advances its own copy).
static thread_local uint32_t loader_start_rom = 0;

void mt_prof_load_begin();
void mt_prof_load_end(uint32_t rom, uint32_t dest, uint32_t size, bool code);

extern "C" void mt_loader_begin(uint32_t rom) {
    loader_start_rom = rom;
    mt_prof_load_begin();
}

extern "C" void mt_on_segment_loaded(uint8_t* rdram, uint32_t rom_end, uint32_t dest, uint32_t size) {
    (void)rdram;
    (void)rom_end;
    uint32_t rom = loader_start_rom;
    uint32_t rom_offset = rom & 0x0FFFFFFF;
    const OverlayInfo* info = nullptr;
    for (const auto& o : overlay_table) {
        if (o.real_rom == rom_offset) { info = &o; break; }
    }
    mt_prof_load_end(rom_offset, dest, size, info != nullptr);
    if (info == nullptr) {  // data segment
        static int logged = 0;
        if (std::getenv("MT_LOG_LOADS") != nullptr && logged++ < 200) {
            fprintf(stderr, "[mt] data segment rom 0x%08X (raw 0x%08X) -> 0x%08X (0x%X bytes)\n", rom_offset, rom, dest, size);
        }
        return;
    }
    fprintf(stderr, "[mt] code segment rom 0x%08X -> 0x%08X (0x%X bytes)\n", rom_offset, dest, size);

    // Code linked outside KSEG0 runs through a TLB mapping onto the buffer it was loaded into.
    const bool tlb = (info->vram & 0xE0000000) != 0x80000000;
    if ((!tlb && dest != info->vram) || size != info->size) {
        fprintf(stderr, "[mt] code segment rom 0x%08X loaded at 0x%08X size 0x%X (expected 0x%08X size 0x%X)\n",
            rom_offset, dest, size, info->vram, info->size);
    }

    auto activate = [&](uint32_t ram) { activate_section(info->vrom, ram, info->size); };

    // seg71: centre the 304x224 screen layout (boot logos etc.) in the 384-wide framebuffer by moving
    // its static viewport {608,448,511,0},{608,448,511,0} at 0x80124648 right by 40 px (trans.x 608 ->
    // 768). The matching scissors are widened by instruction patches in mt.us.toml.
    if (rom_offset == 0x009CD770 && dest == 0x800E8000) {
        constexpr gpr vp = (gpr)(int32_t)0x80124648;
        if (MEM_HU(0, vp) == 608 && MEM_HU(2, vp) == 448 && MEM_HU(8, vp) == 608 && MEM_HU(10, vp) == 448) {
            MEM_H(8, vp) = 768;
        }
    }

    if (tlb) {
        // Registered at its linked address and (last, so it is the relocation base) at the buffer.
        // Calls through any TLB alias or sliding window resolve via LOOKUP_FUNC's TLB translation
        // (include/recomp.h) to the buffer address.
        activate(info->vram);
    }
    activate(dest);
}

namespace {
    constexpr gpr netplay_frame_address = (gpr)(int32_t)0x80061F48;
#ifdef MT_ENABLE_NETPLAY
    mt::netplay::StateCheck court_checkpoint;
#endif

    struct ReplayPad {
        unsigned buttons;
        int x;
        int y;
    };

    struct ReplayFrame {
        ReplayPad pads[4];
    };

    [[noreturn]] void end_replay(int status) {
        // This hook runs on a game thread. Global teardown is owned by the host.
        std::fflush(stderr);
        std::_Exit(status);
    }

    std::vector<ReplayFrame> read_replay(const char* path) {
        std::ifstream input{path};
        std::vector<ReplayFrame> frames;
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            uint32_t frame;
            ReplayFrame sample{};
            if (!(row >> frame >> sample.pads[0].buttons >> sample.pads[0].x >> sample.pads[0].y
                                >> sample.pads[1].buttons >> sample.pads[1].x >> sample.pads[1].y) ||
                frame != frames.size()) {
                fprintf(stderr, "[netreplay] invalid or missing record for guest frame %zu\n", frames.size());
                end_replay(2);
            }
            if ((row >> std::ws) && !row.eof() &&
                (!(row >> sample.pads[2].buttons >> sample.pads[2].x >> sample.pads[2].y
                       >> sample.pads[3].buttons >> sample.pads[3].x >> sample.pads[3].y) ||
                 ((row >> std::ws) && !row.eof()))) {
                fprintf(stderr, "[netreplay] invalid four-player record for guest frame %u\n", frame);
                end_replay(2);
            }
            for (const ReplayPad& pad : sample.pads) {
                if (pad.buttons > 0xFFFF || pad.x < -128 || pad.x > 127 || pad.y < -128 || pad.y > 127) {
                    fprintf(stderr, "[netreplay] controller value out of range at guest frame %u\n", frame);
                    end_replay(2);
                }
            }
            frames.push_back(sample);
        }
        if (frames.empty()) {
            fprintf(stderr, "[netreplay] unable to read any frames from %s\n", path);
            end_replay(2);
        }
        return frames;
    }
}

extern "C" void mt_trace_netplay_guest(uint8_t* rdram, uint32_t pc, uint32_t detail) {
    static const bool enabled = [] {
        const char* value = std::getenv("MT_NETPLAY_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled) return;

    constexpr gpr rng_address = (gpr)(int32_t)0x8005CC10;
    const uint32_t frame = MEM_W(0, netplay_frame_address);
    const uint32_t cursor = MEM_W(0, rng_address);
    const uint32_t rng_word = MEM_W(4, rng_address);
    if (pc == 0x80033874 && detail >= 0x80000000 && detail <= 0x807FFFF6 && detail % 2 == 0) {
        const gpr pads = (gpr)(int32_t)detail;
        fprintf(stderr, "[nettrace] frame=%u pc=%08X rng=%08X/%08X p1=%04X,%d,%d p2=%04X,%d,%d\n",
            frame, pc, cursor, rng_word,
            (unsigned)MEM_HU(0, pads), (int)MEM_B(2, pads), (int)MEM_B(3, pads),
            (unsigned)MEM_HU(6, pads), (int)MEM_B(8, pads), (int)MEM_B(9, pads));
        return;
    }
    fprintf(stderr, "[nettrace] frame=%u pc=%08X rng=%08X/%08X detail=%08X\n",
        frame, pc, cursor, rng_word, detail);
}

extern "C" uint32_t mt_netplay_rng_count(uint8_t* rdram, uint32_t count) {
    static const bool fixed = [] {
        const char* value = std::getenv("MT_NETPLAY_FIXED_RNG");
#ifdef MT_ENABLE_NETPLAY
        if (mt::netplay::role() != mt::netplay::Role::Offline) return true;
#endif
        return value != nullptr && value[0] == '1';
    }();
    if (fixed) {
        // This loop may retry its message wait within a frame. Apply the
        // extra advances once, independent of the host clock and retries.
        static uint32_t last_frame = 0;
        const uint32_t frame = MEM_W(0, netplay_frame_address);
        const uint32_t advances = frame != last_frame ? frame & 7u : 0;
        last_frame = frame;
        count = (count & ~7u) | advances;
    }
    mt_trace_netplay_guest(rdram, 0x80031FF0, count & 7u);
    return count;
}

extern "C" void mt_netplay_read_pads(uint8_t* rdram, uint32_t address) {
    // One row per guest frame, with buttons, X and Y for each port. A frame
    // may poll controls more than once, so hold the sample for every read.
    static const char* replay_path = std::getenv("MT_NETPLAY_REPLAY");
    const bool playing_tape = replay_path != nullptr && replay_path[0] != '\0';
    static const std::vector<ReplayFrame> replay = playing_tape ? read_replay(replay_path) : std::vector<ReplayFrame>{};
#ifdef MT_ENABLE_NETPLAY
    const auto role = mt::netplay::role();
    const bool lan = role != mt::netplay::Role::Offline;
#else
    constexpr bool lan = false;
#endif
    if (playing_tape || lan) {
        const uint32_t frame = MEM_W(0, netplay_frame_address);
        const bool tape_ended = playing_tape && frame >= replay.size();
        if (tape_ended && !lan) {
            fprintf(stderr, "[netreplay] completed at guest frame %u\n", frame);
            end_replay(0);
        }
        unsigned port_count = 2;
#ifdef MT_ENABLE_NETPLAY
        if (lan) port_count = mt::netplay::player_count();
#endif
        if (address < 0x80000000 || address > 0x80800000u - (6 * port_count - 1) || address % 2 != 0) {
            fprintf(stderr, "[netreplay] invalid controller buffer at guest frame %u\n", frame);
            end_replay(2);
        }
        const gpr pads = (gpr)(int32_t)address;
#ifdef MT_ENABLE_NETPLAY
        if (lan) {
            using namespace mt::netplay;
            static uint32_t next_tick = 0;
            static Pads admitted{};
            // Complete a common terminal tick even when the game skipped a
            // controller read just before the end of the tape.
            const uint32_t target = tape_ended ? static_cast<uint32_t>(replay.size()) : frame;
            while (next_tick <= target) {
                Pads local{};
                for (unsigned i = 0; i < pads_per_peer(); ++i) {
                    const unsigned port = first_local_port() + i;
                    const int offset = 6 * port;
                    if (!playing_tape) {
                        local[i] = {MEM_HU(offset, pads), MEM_B(offset + 2, pads), MEM_B(offset + 3, pads)};
                    } else if (next_tick + input_delay < replay.size()) {
                        const auto& sample = replay[next_tick + input_delay].pads[port];
                        local[i] = {uint16_t(sample.buttons), int8_t(sample.x), int8_t(sample.y)};
                    }
                }
                if (!admit(next_tick, local, court_checkpoint, admitted)) end_replay(3);
                ++next_tick;
            }
            if (tape_ended) {
                drain_replay();
                fprintf(stderr, "[netreplay] completed at guest frame %u\n", frame);
                end_replay(0);
            }
            for (unsigned port = 0; port < port_count; ++port) {
                const int offset = 6 * port;
                MEM_HU(offset, pads) = admitted[port].buttons;
                MEM_B(offset + 2, pads) = admitted[port].x;
                MEM_B(offset + 3, pads) = admitted[port].y;
                MEM_B(offset + 4, pads) = 0;
            }
            static const bool trace = std::getenv("MT_NETPLAY_TRACE") != nullptr;
            if (trace && port_count == 4) {
                fprintf(stderr, "[netpads] frame=%u", frame);
                for (unsigned port = 0; port < port_count; ++port) {
                    const auto& pad = admitted[port];
                    fprintf(stderr, " p%u=%04X,%d,%d", port + 1, unsigned(pad.buttons), int(pad.x), int(pad.y));
                }
                fprintf(stderr, "\n");
            }
            mt_trace_netplay_guest(rdram, 0x80033874, address);
            return;
        }
#endif
        for (int port = 0; port < 2; port++) {
            const ReplayPad& sample = replay[frame].pads[port];
            const int offset = 6 * port;
            MEM_HU(offset, pads) = (uint16_t)sample.buttons;
            MEM_B(offset + 2, pads) = (int8_t)sample.x;
            MEM_B(offset + 3, pads) = (int8_t)sample.y;
            MEM_B(offset + 4, pads) = 0;
        }
    }
    mt_trace_netplay_guest(rdram, 0x80033874, address);
}

extern "C" void mt_trace_court_state(uint8_t* rdram) {
    static const bool enabled = std::getenv("MT_NETPLAY_COURT_TRACE") != nullptr;
#ifdef MT_ENABLE_NETPLAY
    const bool lan = mt::netplay::role() != mt::netplay::Role::Offline;
#else
    constexpr bool lan = false;
#endif
    if (!enabled && !lan) return;

    constexpr gpr ball = (gpr)(int32_t)0x8015B0D8;
    constexpr gpr points = (gpr)(int32_t)0x8015344A;
    constexpr gpr player_one = (gpr)(int32_t)0x80157010;
    constexpr gpr player_two = (gpr)(int32_t)0x801571F8;
    static uint32_t step = 0;
    const uint32_t current_step = step++;
    const uint32_t frame = MEM_W(0, netplay_frame_address);
#ifdef MT_ENABLE_NETPLAY
    if (lan && current_step % 30 == 0) {
        // Hash only simulation fields sampled immediately after live ball physics.
        const uint32_t values[] = {
            frame,
            (uint32_t)MEM_W(0x0C, ball), (uint32_t)MEM_W(0x10, ball), (uint32_t)MEM_W(0x14, ball),
            (uint32_t)MEM_W(0x18, ball), (uint32_t)MEM_W(0x1C, ball), (uint32_t)MEM_W(0x20, ball),
            (uint32_t)MEM_W(0x88, ball), (uint32_t)MEM_BU(0x93, ball), (uint32_t)MEM_BU(0x97, ball),
            (uint32_t)MEM_W(0, player_one), (uint32_t)MEM_W(4, player_one), (uint32_t)MEM_W(8, player_one),
            (uint32_t)MEM_W(0, player_two), (uint32_t)MEM_W(4, player_two), (uint32_t)MEM_W(8, player_two),
            (uint32_t)MEM_BU(0, points), (uint32_t)MEM_BU(1, points),
        };
        uint32_t hash = 2166136261u;
        for (uint32_t word : values) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash = (hash ^ ((word >> shift) & 0xffu)) * 16777619u;
            }
        }
        if (mt::netplay::player_count() == 4) {
            for (int port = 2; port < 4; ++port) {
                for (int axis = 0; axis < 3; ++axis) {
                    const uint32_t word = MEM_W(port * 0x1E8 + axis * 4, player_one);
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        hash = (hash ^ ((word >> shift) & 0xffu)) * 16777619u;
                }
            }
        }
        court_checkpoint = {current_step + 1, hash};
    }
#endif
    if (!enabled) return;
    std::fprintf(stderr,
        "[court] step=%u frame=%u pos=%d,%d,%d vel=%08X,%08X,%08X mode=%08X bounces=%u hitter=%u"
        " p1=%d,%d,%d p2=%d,%d,%d points=%u,%u\n",
        current_step, frame,
        (int32_t)MEM_W(0x0C, ball), (int32_t)MEM_W(0x10, ball), (int32_t)MEM_W(0x14, ball),
        (uint32_t)MEM_W(0x18, ball), (uint32_t)MEM_W(0x1C, ball), (uint32_t)MEM_W(0x20, ball),
        (uint32_t)MEM_W(0x88, ball), (unsigned)MEM_BU(0x93, ball), (unsigned)MEM_BU(0x97, ball),
        (int32_t)MEM_W(0, player_one), (int32_t)MEM_W(4, player_one), (int32_t)MEM_W(8, player_one),
        (int32_t)MEM_W(0, player_two), (int32_t)MEM_W(4, player_two), (int32_t)MEM_W(8, player_two),
        (unsigned)MEM_BU(0, points), (unsigned)MEM_BU(1, points));
}
