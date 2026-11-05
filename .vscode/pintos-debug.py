#!/usr/bin/env python3
"""Debug one Pintos test or a range, capturing a separate log for each boot."""

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import re
import shlex
import shutil
import struct
import threading
import select
import tempfile
import uuid
import socket
import subprocess
import sys
import time


GDB_PORT = 57391


def stop_requested(signum, frame):
    raise SystemExit(0)


def test_relative_path(root, test):
    """Find the checker for a VM, COW, or previous-project regression test."""
    if not isinstance(test, str) or "/" in test or "\\" in test:
        raise ValueError(f"Unknown Pintos test: {test}")
    for directory in map(Path, (
        "tests/vm", "tests/vm/cow", "tests/userprog", "tests/filesys/base", "tests/threads",
    )):
        if (root / "pintos" / directory / f"{test}.ck").is_file():
            return directory / test
    raise ValueError(f"Unknown Pintos test: {test}")


def test_launch_spec(root, build, test):
    """Use the official test recipe for arguments, fixtures and kernel flags."""
    target = test_relative_path(root, test).with_suffix(".output").as_posix()
    completed = subprocess.run(
        ["make", "--no-print-directory", "-n", "-W", "os.dsk", target],
        cwd=build, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if completed.returncode:
        raise RuntimeError(f"Cannot prepare {test}: {completed.stderr.strip()}")
    commands = [line for line in completed.stdout.splitlines() if line.startswith("pintos ")]
    if len(commands) != 1:
        raise RuntimeError(f"Cannot find the official launch command for {test}.")
    words = shlex.split(commands[0])
    separator = words.index("--")
    options = words[1:separator]
    kernel_args = words[separator + 1:words.index("<", separator)]
    files = []
    fs_size = 10
    memory = 20
    swap_size = None
    for index, option in enumerate(options):
        if option == "-p":
            host, _, guest = options[index + 1].partition(":")
            files.append((build / host, guest or Path(host).name))
        elif option.startswith("--fs-disk="):
            fs_size = int(option.split("=", 1)[1])
        elif option == "-m":
            memory = int(options[index + 1])
        elif option.startswith("--swap-disk="):
            swap_size = int(option.split("=", 1)[1])
    return {
        "kernel_args": kernel_args, "files": files, "fs_size": fs_size,
        "memory": memory, "swap_size": swap_size,
    }


def check_result(root, build, test):
    test_path = test_relative_path(root, test)
    output = build / test_path.with_suffix(".output")
    result = build / test_path.with_suffix(".result")
    result.unlink(missing_ok=True)
    if not output.exists():
        print(f"[NOT RUN] {test}: no output from this debug session.", flush=True)
        return 0

    content = output.read_text(errors="replace")
    if "Powering off" not in content and not any(
        marker in content for marker in ("PANIC", "FAIL", "TIMEOUT")
    ):
        print(f"[STOPPED] {test}: debugging ended before the test finished.", flush=True)
        print(f"Output: {output}", flush=True)
        return 0

    completed = subprocess.run(
        ["perl", f"-I{root / 'pintos'}", str(root / 'pintos' / test_path.with_suffix('.ck')),
         str(test_path), str(test_path.with_suffix('.result'))],
        cwd=build,
        env={**os.environ, "PATH": f"{root / 'pintos/utils'}:{os.environ.get('PATH', '')}"},
    )
    if completed.returncode or not result.exists():
        print(f"[ERROR] {test}: the Pintos checker could not produce a result.", flush=True)
        return 1
    verdict = result.read_text().splitlines()[0]
    print(f"\n[{verdict}] {test}", flush=True)
    print(f"Result: {result}", flush=True)
    return 0 if verdict == "PASS" else 1



def resolve_tests(root, selection, settings):
    """Resolve a test name, an order number, or an inclusive range such as 1~6."""
    order = settings.get("test_order", {})
    if type(selection) is int:
        numbers = [selection]
    elif isinstance(selection, str) and selection.strip().isdigit():
        numbers = [int(selection.strip())]
    elif isinstance(selection, str) and "~" in selection:
        match = re.fullmatch(r"\s*(\d+)\s*~\s*(\d+)\s*", selection)
        if not match:
            raise ValueError("Use an inclusive range such as 1~6.")
        first, last = map(int, match.groups())
        if first > last:
            raise ValueError("The range must be in ascending order.")
        if last - first + 1 > len(order):
            raise ValueError("The range contains numbers outside test_order.")
        numbers = range(first, last + 1)
    elif isinstance(selection, str):
        numbers = None
        tests = [selection]
    else:
        raise ValueError("test must be a test name, an order number, or a range.")
    if numbers is not None:
        try:
            tests = [order[str(number)] for number in numbers]
        except (KeyError, TypeError) as error:
            raise ValueError(f"Unknown number in test_order: {error}") from error
    for test in tests:
        test_relative_path(root, test)
    return tests


class DebugVM:
    def __init__(self, root, build, output_dir, test, sequence=False, icount=None):
        self.root = root
        self.build = build
        self.output_dir = output_dir
        self.test = test
        self.test_path = output_dir / test_relative_path(root, test)
        self.sequence = sequence
        self.icount = icount
        self.process = None
        self.output = None
        self.errors = None
        self.readers = []
        self.lock = threading.Lock()
        # Only this copied image is changed; the built os.dsk stays intact.
        self.disk = output_dir / "boot.dsk"
        self.filesys_disk = output_dir / "filesys.dsk"
        self.scratch_disk = output_dir / "scratch.dsk"
        self.swap_disk = output_dir / "swap.dsk"
        self.specs = {}

    def prepare_test(self, test):
        test_path = test_relative_path(self.root, test)
        if test not in self.specs:
            self.specs[test] = test_launch_spec(self.root, self.build, test)
        spec = self.specs[test]
        missing = [str(host.relative_to(self.build)) for host, _ in spec["files"]
                   if not host.is_file()]
        if missing:
            completed = subprocess.run(
                ["make", "--no-print-directory", *missing], cwd=self.build,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            if completed.returncode:
                raise RuntimeError(f"Cannot prepare files for {test}: {completed.stderr.strip()}")
        kernel_args = spec["kernel_args"]
        run_index = kernel_args.index("run")
        puts = [word for _, guest in spec["files"] for word in ("put", guest)]
        arguments = kernel_args[:run_index] + puts + kernel_args[run_index:]
        command_line = ("\0".join(arguments) + "\0").encode()
        if len(command_line) > 128:
            raise ValueError("Pintos kernel command line exceeds 128 bytes.")
        # Every boot gets a clean file system, including in a test sequence.
        with self.filesys_disk.open("wb") as disk:
            disk.truncate(0xfc000 * spec["fs_size"])
            disk.flush()
            os.fsync(disk.fileno())
        if spec["swap_size"] is not None:
            with self.swap_disk.open("wb") as disk:
                disk.truncate(0xfc000 * spec["swap_size"])
        # This is the PUT transfer format used by pintos/utils/pintos.
        with self.scratch_disk.open("wb") as disk:
            for host, _ in spec["files"]:
                data = host.read_bytes()
                disk.write(b"PUT\0" + struct.pack("<I", len(data)) + bytes(504))
                disk.write(data)
                disk.write(bytes((-len(data)) % 512))
            if not spec["files"]:
                disk.write(bytes(512))
            # QEMU keeps the disk capacity across system_reset. Reserve enough
            # for all fixtures in the 10 MB test file system on every boot.
            if disk.tell() > 16 * 1024 * 1024:
                raise ValueError("Test transfer files exceed the 16 MB scratch disk.")
            disk.truncate(16 * 1024 * 1024)
            disk.flush()
            os.fsync(disk.fileno())
        with self.disk.open("r+b") as disk:
            disk.seek(0x17a)
            disk.write(struct.pack("<I", len(arguments)))
            disk.write(command_line.ljust(128, b"\0"))
            disk.flush()
            os.fsync(disk.fileno())
        with self.lock:
            if self.output:
                self.output.close()
                self.errors.close()
            self.test = test
            self.test_path = self.output_dir / test_path
            self.test_path.parent.mkdir(parents=True, exist_ok=True)
            self.test_path.with_suffix(".result").unlink(missing_ok=True)
            self.output = self.test_path.with_suffix(".output").open("wb")
            self.errors = self.test_path.with_suffix(".errors").open("wb")

    def pump(self, stream, is_output):
        while True:
            chunk = os.read(stream.fileno(), 4096)
            if not chunk:
                break
            with self.lock:
                target = self.output if is_output else self.errors
                target.write(chunk)
                target.flush()
            if is_output:
                print(chunk.decode(errors="replace"), end="", flush=True)

    def start(self):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                probe.bind(("127.0.0.1", GDB_PORT))
            except OSError as error:
                raise RuntimeError(f"GDB port {GDB_PORT} is already in use. Stop the previous debug session.") from error
        shutil.copyfile(self.build / "os.dsk", self.disk)
        self.prepare_test(self.test)
        command = [
            "qemu-system-x86_64", "-nographic",
            "-gdb", f"tcp:127.0.0.1:{GDB_PORT}", "-S",
            "-drive", f"file={self.disk},format=raw,index=0,media=disk",
            "-drive", f"file={self.filesys_disk},format=raw,index=1,media=disk",
            "-drive", f"file={self.scratch_disk},format=raw,index=2,media=disk",
            "-cpu", "qemu64", "-m", str(self.specs[self.test]["memory"]),
            "-serial", "mon:stdio",
        ]
        if self.specs[self.test]["swap_size"] is not None:
            command.extend([
                "-drive", f"file={self.swap_disk},format=raw,index=3,media=disk",
            ])
        if self.icount is not None:
            command.extend(["-icount", self.icount])
        if not self.sequence:
            command.append("-no-reboot")
        self.process = subprocess.Popen(
            command, cwd=self.build, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=True,
        )
        for stream, is_output in ((self.process.stdout, True), (self.process.stderr, False)):
            reader = threading.Thread(target=self.pump, args=(stream, is_output), daemon=True)
            reader.start()
            self.readers.append(reader)
        # Detect the listening socket without connecting to the paused target.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"{self.test}: Pintos exited before its GDB server became ready.")
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind(("127.0.0.1", GDB_PORT))
                except OSError as error:
                    if error.errno == errno.EADDRINUSE:
                        return
                    raise
            time.sleep(0.05)
        raise RuntimeError("Timed out waiting for the Pintos GDB server.")

    def show_output(self):
        # Reader threads stream output while GDB stops or resets the VM.
        pass

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGINT)
            except ProcessLookupError:
                pass
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait()
        for reader in self.readers:
            reader.join(timeout=3)
        self.readers = []
        if self.process:
            self.process.stdout.close()
            self.process.stderr.close()
        with self.lock:
            if self.output:
                self.output.close()
                self.errors.close()
                self.output = None
                self.errors = None
        self.disk.unlink(missing_ok=True)
        self.filesys_disk.unlink(missing_ok=True)
        self.scratch_disk.unlink(missing_ok=True)
        self.swap_disk.unlink(missing_ok=True)


def write_session(output_dir, session):
    temporary = output_dir / "session.json.tmp"
    temporary.write_text(json.dumps(session, indent=2) + "\n")
    temporary.replace(output_dir / "session.json")


def start_debug(root, build, output_dir, tests, icount=None):
    specs = {test: test_launch_spec(root, build, test) for test in tests}
    hardware = {(spec["memory"], spec["swap_size"]) for spec in specs.values()}
    if len(hardware) > 1:
        raise RuntimeError(
            "These tests require different RAM or swap sizes. "
            "Run them separately, or select a range with matching RAM and swap settings."
        )
    signal.signal(signal.SIGTERM, stop_requested)
    signal.signal(signal.SIGINT, stop_requested)
    output_dir.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    run_dir = output_dir / f"sequence-{token[:8]}" if len(tests) > 1 else output_dir
    run_dir.mkdir(parents=True, exist_ok=True)
    kernel = run_dir / "kernel.o"
    if not kernel.exists() and not kernel.is_symlink():
        kernel.symlink_to(build / "kernel.o")
    session = {
        "tests": tests, "index": 0, "status": "starting",
        "output_subdir": run_dir.relative_to(output_dir).as_posix(),
        "token": token, "icount": icount,
    }
    vm = None
    with tempfile.TemporaryDirectory(prefix="pintos-debug-") as control_dir, \
            socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        session["socket"] = str(Path(control_dir) / "control.sock")
        server.bind(session["socket"])
        server.listen(1)
        try:
            vm = DebugVM(
                root, build, run_dir, tests[0],
                sequence=len(tests) > 1, icount=icount,
            )
            vm.specs = specs
            vm.start()
            session["status"] = "running"
            write_session(output_dir, session)
            (output_dir / "last-test").write_text(tests[0] + "\n")
            print(f"\n[1/{len(tests)}] {tests[0]}", flush=True)
            print("Pintos GDB server ready", flush=True)
            while vm.process.poll() is None:
                vm.show_output()
                readable, _, _ = select.select([server], [], [], 0.05)
                if not readable:
                    continue
                with server.accept()[0] as connection:
                    connection.settimeout(5)
                    request = b""
                    while not request.endswith(b"\n") and len(request) < 4096:
                        chunk = connection.recv(4096 - len(request))
                        if not chunk:
                            break
                        request += chunk
                    try:
                        command = json.loads(request)
                        if command.get("token") != token or command.get("index") != session["index"]:
                            raise ValueError("This request does not belong to the current debug session.")
                        if command.get("action") != "next" or session["index"] + 1 >= len(tests):
                            raise ValueError("There is no next test in this sequence.")
                        # The UART reader may still be catching up to GDB's shutdown stop.
                        deadline = time.monotonic() + 2
                        # GDB requests the next boot only at the shutdown breakpoint.
                        # Panic output can stop before the final UART shutdown message.
                        shutdown_markers = ("Powering off", "Kernel PANIC", ") FAIL:")
                        while not any(
                            marker in vm.test_path.with_suffix(".output").read_text(errors="replace")
                            for marker in shutdown_markers
                        ):
                            if time.monotonic() >= deadline:
                                raise ValueError("The current test has not reached shutdown.")
                            time.sleep(0.01)
                    except (ValueError, TypeError, AttributeError) as error:
                        connection.sendall((json.dumps({"error": str(error)}) + "\n").encode())
                        continue
                    # At shutdown, prepare the next boot before GDB resets the VM.
                    session["index"] += 1
                    test = tests[session["index"]]
                    vm.prepare_test(test)
                    write_session(output_dir, session)
                    (output_dir / "last-test").write_text(test + "\n")
                    print(f"\n[{session['index'] + 1}/{len(tests)}] {test}", flush=True)
                    connection.sendall((json.dumps({
                        "index": session["index"], "test": test,
                        "last": session["index"] == len(tests) - 1,
                    }) + "\n").encode())
            vm.show_output()
            if session["index"] < len(tests) - 1:
                session["status"] = "interrupted"
                print("The sequence ended before all tests ran.", flush=True)
                return 1
            session["status"] = "finished"
            return vm.process.returncode
        finally:
            if vm is not None:
                vm.stop()
            if session["status"] in ("starting", "running"):
                session["status"] = "stopped"
            write_session(output_dir, session)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check the captured debug output")
    parser.add_argument("--icount", help="Override QEMU virtual instruction clock settings")
    parser.add_argument("test", nargs="?", help="Override the test name, number, or range in pintos-debug.json")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = root / "pintos/vm/build"
    output_dir = build / "debug"
    if args.check and args.test is None:
        manifest = output_dir / "session.json"
        if manifest.is_file():
            try:
                session = json.loads(manifest.read_text())
                tests = session["tests"]
                if not isinstance(tests, list) or not tests:
                    raise ValueError("No tests recorded in this session.")
                for test in tests:
                    resolve_tests(root, test, {})
                run_dir = (output_dir / session["output_subdir"]).resolve()
                if not run_dir.is_relative_to(output_dir.resolve()):
                    raise ValueError("The recorded output directory is outside the debug directory.")
            except (OSError, ValueError, KeyError, TypeError) as error:
                parser.error(f"Cannot read the recorded debug session: {error}")
            results = [check_result(root, run_dir, test) for test in tests]
            return int(any(results))
        last_test = output_dir / "last-test"
        if last_test.is_file():
            args.test = last_test.read_text().strip()
    settings = {}
    if args.test is None or args.test.strip().isdigit() or "~" in args.test:
        config = root / ".vscode/pintos-debug.json"
        try:
            settings = json.loads(config.read_text())
            selection = settings["test"] if args.test is None else args.test
        except (OSError, ValueError, KeyError, TypeError) as error:
            parser.error(f"Cannot read test setting from {config}: {error}")
    else:
        selection = args.test
        config = root / ".vscode/pintos-debug.json"
        if config.is_file():
            try:
                settings = json.loads(config.read_text())
            except (OSError, ValueError, TypeError) as error:
                parser.error(f"Cannot read test setting from {config}: {error}")
    try:
        tests = resolve_tests(root, selection, settings)
    except ValueError as error:
        parser.error(str(error))
    if args.check:
        return int(any([check_result(root, output_dir, test) for test in tests]))
    icount = args.icount
    if icount is not None and (not isinstance(icount, str) or not icount.strip()):
        parser.error("icount must be a nonempty QEMU settings string or null")
    try:
        return start_debug(root, build, output_dir, tests, icount)
    except (OSError, RuntimeError) as error:
        print(f"[ERROR] {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
