"""Load user symbols and keep GDB connected across selected Pintos tests."""

import json
from pathlib import Path
import re
import socket

import gdb


class PintosSequence:
    def __init__(self, session):
        self.session = session
        self.breakpoint = None
        self.build = Path(gdb.current_progspace().filename).resolve().parent
        self.user_symbol_file = None
        gdb.execute("set breakpoint pending on", to_string=True)
        self.load_user_symbols(session["tests"][session["index"]])
        if len(session["tests"]) < 2:
            return
        source = Path(gdb.lookup_global_symbol("power_off").symtab.fullname())
        shutdown_lines = [
            number for number, line in enumerate(source.read_text().splitlines(), 1)
            if re.search(r"\boutw\s*\(\s*0x604\s*,\s*0x2000\s*\)", line)
        ]
        if len(shutdown_lines) != 1:
            raise gdb.GdbError("Cannot locate the Pintos shutdown instruction for the test sequence.")
        self.breakpoint = gdb.Breakpoint(
            f"{source}:{shutdown_lines[0]}",
            type=gdb.BP_HARDWARE_BREAKPOINT, internal=True,
        )
        # Commands run after stopping, so switching targets is safe here.
        self.breakpoint.commands = "silent\npython _pintos_sequence.advance()\ncontinue"
        gdb.write(f"Pintos sequence: {' -> '.join(session['tests'])}\n")

    def unload_user_symbols(self):
        if self.user_symbol_file is not None:
            gdb.execute(
                f"remove-symbol-file {json.dumps(str(self.user_symbol_file))}",
                to_string=True,
            )
            self.user_symbol_file = None

    def load_user_symbols(self, test):
        self.unload_user_symbols()
        for directory in ("tests/vm", "tests/vm/cow", "tests/userprog", "tests/filesys/base"):
            executable = self.build / directory / test
            if executable.is_file():
                # ELF section addresses identify the user program's load address.
                gdb.execute(
                    f"add-symbol-file {json.dumps(str(executable))}",
                    to_string=True,
                )
                self.user_symbol_file = executable
                gdb.write(f"Pintos user symbols: {test}\n")
                return

    def advance(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(20)
            connection.connect(self.session["socket"])
            connection.sendall((json.dumps({
                "action": "next", "token": self.session["token"],
                "index": self.session["index"],
            }) + "\n").encode())
            response = b""
            while not response.endswith(b"\n") and len(response) < 4096:
                chunk = connection.recv(4096 - len(response))
                if not chunk:
                    raise gdb.GdbError("The Pintos sequence launcher stopped before the next test was ready.")
                response += chunk
        reply = json.loads(response)
        if "error" in reply:
            raise gdb.GdbError(reply["error"])
        self.session["index"] = reply["index"]
        if reply["last"]:
            self.breakpoint.enabled = False
        gdb.write(f"\nPintos next test: {reply['test']}\n")
        self.load_user_symbols(reply["test"])
        gdb.execute("monitor system_reset", to_string=True)
        gdb.execute("maintenance flush register-cache", to_string=True)


class PintosSequenceCommand(gdb.Command):
    def __init__(self):
        super().__init__("pintos-sequence", gdb.COMMAND_RUNNING)

    def invoke(self, argument, from_tty):
        global _pintos_sequence
        arguments = gdb.string_to_argv(argument)
        if len(arguments) != 1:
            raise gdb.GdbError("Usage: pintos-sequence <session.json>")
        session = json.loads(Path(arguments[0]).read_text())
        if _pintos_sequence:
            if _pintos_sequence.breakpoint:
                _pintos_sequence.breakpoint.delete()
            _pintos_sequence.unload_user_symbols()
        _pintos_sequence = PintosSequence(session)


_pintos_sequence = None
PintosSequenceCommand()
