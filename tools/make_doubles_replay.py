from argparse import ArgumentParser
from pathlib import Path


def main():
    parser = ArgumentParser(description="Generate a four-player exhibition replay.")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with args.output.open("w", encoding="ascii") as tape:
        for frame in range(1660):
            pads = [0, 0, 0, 0]
            if any(t <= frame < t + 3 for t in (90, 180, 260)):
                pads[0] = 0x1000
            if any(t <= frame < t + 3 for t in (330, 348, 366)):
                pads[0] = 0x0100
            if frame >= 410 and frame % 34 < 3:
                pads[0] = 0x8000
            for port, start in enumerate((476, 508, 544), 1):
                if frame >= start and frame % 33 < 3:
                    pads[port] = 0x8000
            if frame >= 1000:
                pads = [0x8000 if frame % every < 2 and (frame < 1355 or port >= 2) else 0
                        for port, every in enumerate((11, 7, 9, 13))]
            print(frame, *[value for buttons in pads for value in (buttons, 0, 0)], file=tape)


if __name__ == "__main__":
    main()
