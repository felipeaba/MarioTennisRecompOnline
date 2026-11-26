"""Replay two Windows game instances over IPv4/UDP and compare their court state."""

from argparse import ArgumentParser
from itertools import zip_longest
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


EXE = "MarioTennisRecompiled.exe"
ROM = "mt.n64.us.z64"


def prepare_instance(build, directory):
    directory.mkdir()
    files = [build / EXE, build / ROM, build / "recompcontrollerdb.txt"]
    files.extend(build.glob("*.dll"))
    for file in files:
        shutil.copy2(file, directory)
    for name in ("assets", "shaders", "slang-shaders"):
        source = build / name
        if source.is_dir():
            shutil.copytree(source, directory / name)
        elif name == "assets":
            raise FileNotFoundError(source)
    # The port switches its working directory to the executable's directory.
    # A marker here isolates settings and saves from the installation and player.
    (directory / "portable.txt").touch()


def local_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_pair(instances, tape, timeout, logs_dir):
    port = local_port()
    common_env = {key: value for key, value in os.environ.items() if not key.upper().startswith("MT_")}
    common_env.update({
        "MT_AUTOSTART": "1",
        "MT_PLAYERS": "2",
        "MT_NETPLAY_REPLAY": str(tape),
        "MT_NETPLAY_TRACE": "1",
        "MT_NETPLAY_FIXED_RNG": "1",
        "MT_NETPLAY_COURT_TRACE": "1",
    })
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    processes = []
    log_files = [instance / "game.log" for instance in instances]
    saves = []
    for index, instance in enumerate(instances):
        save = instance / "saves" / "mt.n64.us.bin"
        save.parent.mkdir()
        contents = bytes([0xA5 + index]) * 2048
        save.write_bytes(contents)
        saves.append((save, contents))
    try:
        for index, instance in enumerate(instances):
            role = {"MT_NETPLAY_HOST": str(port)} if index == 0 else {"MT_NETPLAY_JOIN": f"127.0.0.1:{port}"}
            env = common_env | role | {"MT_LOG": str(log_files[index])}
            process = subprocess.Popen([str(instance / EXE)], cwd=instance, env=env, startupinfo=startup)
            processes.append(process)

        deadline = time.monotonic() + timeout
        while any(process.poll() is None for process in processes):
            if any(process.returncode not in (None, 0) for process in processes):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"The replay exceeded {timeout:g} seconds")
            time.sleep(0.05)

        logs = [path.read_text(errors="replace").splitlines() if path.is_file() else [] for path in log_files]
        for instance, process, log in zip(instances, processes, logs):
            if process.returncode not in (None, 0):
                tail = "\n".join(log[-12:])
                raise RuntimeError(f"{instance.name} exited with status {process.returncode}\n{tail}")
        for save, contents in saves:
            if save.read_bytes() != contents or set(save.parent.iterdir()) != {save}:
                raise RuntimeError(f"Netplay modified persistent saves in {save.parent}")
        return logs
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        if logs_dir is not None:
            logs_dir.mkdir(parents=True, exist_ok=True)
            for instance, file in zip(instances, log_files):
                if file.is_file():
                    shutil.copy2(file, logs_dir / f"{instance.name}.log")


def verify_rally(trace):
    pattern = re.compile(
        r"frame=(\d+) pos=-?\d+,-?\d+,(-?\d+) vel=[0-9A-F]+,[0-9A-F]+,([0-9A-F]+)"
        r" mode=([0-9A-F]+) bounces=(\d+) hitter=(\d+).* points=(\d+),(\d+)$"
    )
    serve = serve_crossing = returned = return_crossing = None
    for line in trace:
        match = pattern.search(line)
        if match is None:
            raise RuntimeError(f"Invalid court sample: {line}")
        frame, z, speed, mode, _bounces, hitter, point_one, point_two = match.groups()
        frame, z, hitter, points = int(frame), int(z), int(hitter), (int(point_one), int(point_two))
        if points != (0, 0):
            if return_crossing is None or points != (0, 1):
                raise RuntimeError("The first point ended without the expected return and score")
            return serve, serve_crossing, returned, return_crossing, frame
        if int(mode, 16) != 2:
            continue
        forward = int(speed, 16) < 0x80000000
        if serve is None and hitter == 0 and z > 0 and not forward:
            serve = frame
        if serve is not None and serve_crossing is None and hitter == 0 and z < 0 and not forward:
            serve_crossing = frame
        if serve_crossing is not None and returned is None and hitter == 1 and z < 0 and forward:
            returned = frame
        if returned is not None and return_crossing is None and hitter == 1 and z > 0 and forward:
            return_crossing = frame
    raise RuntimeError("The replay did not complete the expected first point")


def compare_logs(logs):
    traces = []
    for role, log in zip(("host", "guest"), logs):
        if not any(line.startswith("[netplay] compatibility verified") for line in log):
            raise RuntimeError(f"{role}: missing compatibility handshake")
        if not any(line.startswith("[netplay] court checkpoint") for line in log):
            raise RuntimeError(f"{role}: no in-game state checkpoints were verified")
        if not any(line.startswith("[netreplay] completed") for line in log):
            raise RuntimeError(f"{role}: the game did not complete the replay")
        trace = [line for line in log if line.startswith("[court] ")]
        if not trace:
            raise RuntimeError(f"{role}: the replay did not reach the court")
        traces.append(trace)

    for index, (host, guest) in enumerate(zip_longest(*traces), 1):
        if host != guest:
            raise RuntimeError(f"First court difference at sample {index}\nHost  {host}\nGuest {guest}")
    serve, serve_cross, returned, return_cross, score = verify_rally(traces[0])
    print(f"Host and guest completed the replay with {len(traces[0])} identical court samples.")
    print(f"Serve {serve}, crossing {serve_cross}, return {returned}, crossing {return_cross}, score 0-15 at {score}.")


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="port build directory containing the locally imported ROM")
    parser.add_argument("tape", type=Path, help="two-controller guest-frame input replay")
    parser.add_argument("--timeout", type=float, default=180, help="total time limit in seconds")
    parser.add_argument("--logs", type=Path, help="copy host and guest logs to this directory")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This test requires the Windows build")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    build = args.build.resolve(strict=True)
    tape = args.tape.resolve(strict=True)
    try:
        with tempfile.TemporaryDirectory(prefix="mt-loop-", dir=build.parent) as temp:
            root = Path(temp)
            instances = [root / "host", root / "guest"]
            for instance in instances:
                prepare_instance(build, instance)
            compare_logs(run_pair(instances, tape, args.timeout, args.logs))
    except (OSError, RuntimeError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
