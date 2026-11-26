from argparse import ArgumentParser
from pathlib import Path
import os
import re
import runpy
import socket
import subprocess
import tempfile
import time


def wait_for_log(log, message, process, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log.exists() and message in log.read_text(errors="replace"):
            return
        if process.poll() is not None:
            break
        time.sleep(.03)
    raise RuntimeError(f"{log} did not report {message!r}")


def run(build, tape, layout, logs, require_point, reject_duplicate):
    helper = runpy.run_path(str(Path(__file__).with_name("test_netplay_loopback.py")))
    logs.mkdir(parents=True, exist_ok=True)
    rows = [list(map(int, line.split())) for line in tape.read_text().splitlines()]
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("MT_")}
    env.update(MT_NETPLAY_MODE=layout, MT_NETPLAY_REPLAY=str(tape),
               MT_NETPLAY_TRACE="1", MT_NETPLAY_COURT_TRACE="1")
    count = 4 if layout == "1+1+1+1" else 2
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(("127.0.0.1", 0))
        port = udp.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="mt-doubles-", dir=build.parent) as scratch:
        games = []
        processes = []
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE

        def launch(seat, label):
            path = Path(scratch) / label
            helper["prepare_instance"](build, path)
            log = logs / f"{label}.log"
            local = env | {"MT_LOG": str(log)}
            if seat == 0:
                local["MT_NETPLAY_HOST"] = str(port)
            else:
                local.update(MT_NETPLAY_JOIN=f"127.0.0.1:{port}", MT_NETPLAY_SEAT=str(seat + 1))
            process = subprocess.Popen([str(path / helper["EXE"])], cwd=path, env=local, startupinfo=startup)
            processes.append(process)
            return process, log

        try:
            for seat in range(count):
                if seat == 2 and reject_duplicate:
                    wait_for_log(games[0][1], "[netplay] lobby occupancy 2/4", games[0][0])
                    duplicate, log = launch(1, "duplicate-p2")
                    wait_for_log(log, "[netplay] seat already occupied, requested player 2", duplicate)
                    assert duplicate.wait(timeout=8) != 0, "Duplicate seat was accepted"
                    print("Duplicate P2 rejected; the original P2 stayed in the room", flush=True)
                games.append(launch(seat, f"peer-{seat}"))

            deadline = time.monotonic() + 210
            while any(peer.poll() is None for peer, _ in games):
                if any(peer.returncode not in (0, None) for peer, _ in games) or time.monotonic() > deadline:
                    raise RuntimeError([(peer.poll(), str(log)) for peer, log in games])
                time.sleep(.05)

            samples = []
            checkpoints = []
            for index, (peer, log) in enumerate(games):
                text = log.read_text(errors="replace")
                port_start = index * (1 if count == 4 else 2) + 1
                assert peer.returncode == 0 and f"compatibility verified, local player {port_start}," in text \
                    and "[netreplay] completed" in text, (peer.returncode, log)
                inputs = 0
                for line in text.splitlines():
                    if not line.startswith("[netpads] "):
                        continue
                    frame = int(re.search(r"frame=(\d+)", line)[1])
                    actual = []
                    for buttons, x, y in re.findall(r"p[1-4]=([0-9A-F]+),(-?\d+),(-?\d+)", line):
                        actual.extend((int(buttons, 16), int(x), int(y)))
                    assert actual == rows[frame][1:], (log, frame, actual, rows[frame])
                    inputs += 1
                assert inputs >= len(rows) - 4, (log, inputs)
                court = [line for line in text.splitlines() if line.startswith("[court] ")]
                checkpoint = [line for line in text.splitlines() if line.startswith("[netplay] court checkpoint ")]
                samples.append(court)
                checkpoints.append(checkpoint)
                print(f"{log.name}: {inputs} exact pad reads, {len(court)} court samples, {len(checkpoint)} checkpoints",
                      flush=True)

            assert all(items == samples[0] for items in samples[1:]), "Court traces diverged"
            assert all(items == checkpoints[0] for items in checkpoints[1:]), "Court checkpoints diverged"
            if require_point:
                assert checkpoints[0], "Expected synchronized court checkpoints"
                assert any(not line.endswith("points=0,0") for line in samples[0]), "Expected a completed point"
                assert all(any(f"hitter={player} " in line for line in samples[0]) for player in (2, 3)), \
                    "Expected player 3 and player 4 to strike the ball"
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()


if __name__ == "__main__":
    parser = ArgumentParser(description="Test four controller ports over UDP on Windows.")
    parser.add_argument("build", type=Path)
    parser.add_argument("tape", type=Path)
    parser.add_argument("layout", choices=["2+2", "1+1+1+1"])
    parser.add_argument("logs", type=Path)
    parser.add_argument("--require-point", action="store_true")
    parser.add_argument("--reject-duplicate", action="store_true")
    args = parser.parse_args()
    if args.reject_duplicate and args.layout != "1+1+1+1":
        parser.error("--reject-duplicate requires the four-PC layout")
    run(args.build.resolve(), args.tape.resolve(), args.layout, args.logs.resolve(),
        args.require_point, args.reject_duplicate)
