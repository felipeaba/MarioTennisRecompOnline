"""Generate the deterministic first point used by the Windows LAN test."""
from argparse import ArgumentParser
from pathlib import Path


def replay_rows(frame_count):
    for frame in range(frame_count):
        player_one = 0
        player_two = 0
        # Skip the opening, select 2 Players, and accept Mario / Peach and
        # the default singles settings and court.
        if any(start <= frame < start + 3 for start in (90, 180, 260)):
            player_one = 0x1000
        if 330 <= frame < 333:
            player_one = 0x0100
        if frame >= 380 and frame % 32 < 3:
            player_one = 0x8000
        if 472 <= frame < 475:
            player_two = 0x0100
        if frame >= 478 and frame % 32 < 3:
            player_two = 0x8000
        # Short A pulses produce a serve and a return from the two seats.
        if frame >= 950:
            player_one = 0x8000 if frame % 11 < 2 else 0
            player_two = 0x8000 if frame % 7 < 2 else 0
        yield frame, player_one, 0, 0, player_two, 0, 0


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frames", type=int, default=1080,
                        help="continue the same input pattern beyond the first point")
    args = parser.parse_args()
    if args.frames < 1080:
        parser.error("--frames must be at least 1080")
    with args.output.open("w", encoding="ascii") as tape:
        for row in replay_rows(args.frames):
            print(*row, file=tape)


if __name__ == "__main__":
    main()
