#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Robert Wendt <tffb4szhgc@privaterelay.appleid.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise real CAP negotiation and crash/restart catch-up on a local fixture.

Usage: python3 tests/integration/irc-chathistory.py build-history
No external IRC server, account, or credentials are used.
"""

# This intentionally linear fixture mirrors one IRC exchange.
# ruff: noqa: C901,PLR0912,PLR0915,S101,S603,T201

import os
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CAPS = "batch server-time message-tags draft/chathistory"


def run(build_dir: str) -> None:
    """Run two clients against a deterministic local server, sharing saved state."""
    build = Path(build_dir).resolve()
    binary = build / "src/gui/curses/headless/weechat-headless"
    env = dict(os.environ, WEECHAT_EXTRA_LIBDIR=str(build / "src"))
    with (
        tempfile.TemporaryDirectory(prefix="weechat-history-") as home,
        socket.socket() as listener,
    ):
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(15)
        port = listener.getsockname()[1]
        processes = []
        try:
            for iteration in range(2):
                commands = "/connect fixture"
                if iteration == 0:
                    commands = (
                        f"/server add fixture 127.0.0.1/{port} -notls "
                        "-nicks=alice -autojoin=#history; "
                        "/set irc.server.fixture.capabilities *,draft/chathistory; "
                        "/save; /connect fixture"
                    )
                log = (Path(home) / f"process-{iteration}.log").open("w+", encoding="utf-8")
                process = subprocess.Popen(
                    [str(binary), "-a", "-d", home, "-P", "irc", "--stdout", "-r", commands],
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                processes.append(process)
                try:
                    connection, _ = listener.accept()
                except TimeoutError:
                    log.seek(0)
                    raise AssertionError("Client failed to connect:\n" + log.read()) from None
                with connection:
                    deadline = time.monotonic() + 15
                    pending = b""
                    requested = False
                    acknowledged = False
                    welcomed = False
                    registered = set()

                    def send(line: str, connection: socket.socket = connection) -> None:
                        connection.sendall((line + "\r\n").encode())

                    while time.monotonic() < deadline and not requested:
                        if not select.select([connection], [], [], 0.2)[0]:
                            continue
                        packet = connection.recv(65536)
                        assert packet, "Client disconnected unexpectedly"
                        pending += packet
                        while b"\r\n" in pending:
                            raw, pending = pending.split(b"\r\n", 1)
                            line = raw.decode()
                            command = line.split(" ")[0]
                            if line.startswith("CAP LS"):
                                send(":fixture CAP * LS :" + CAPS)
                            elif line.startswith("CAP REQ :"):
                                selected = line.split(":", 1)[1].split()
                                assert set(selected) == set(CAPS.split()), selected
                                send(":fixture CAP * ACK :" + " ".join(selected))
                                acknowledged = True
                            elif command in ("NICK", "USER") or line == "CAP END":
                                registered.add("END" if line == "CAP END" else command)
                            elif command == "JOIN":
                                send(":alice!user@fixture JOIN #history")
                                send(":fixture 353 alice = #history :alice bob")
                                send(":fixture 366 alice #history :End of names")
                            elif command == "CHATHISTORY":
                                assert acknowledged, "History requested before CAP ACK"
                                expected = (
                                    "CHATHISTORY LATEST #history * 2"
                                    if iteration == 0
                                    else "CHATHISTORY AFTER #history msgid=b 2"
                                )
                                assert line == expected, (line, expected)
                                requested = True
                                if iteration == 0:
                                    send("@draft/chathistory-end :fixture BATCH +h chathistory #history")
                                    send("@time=2026-10-13T10:00:02.000Z;msgid=b :bob!u@h PRIVMSG #history :live")
                                    send(
                                        "@batch=h;time=2026-10-13T10:00:02.000Z;msgid=b "
                                        ":bob!u@h PRIVMSG #history :live",
                                    )
                                    send(
                                        "@batch=h;time=2026-10-13T10:00:01.000Z;msgid=a "
                                        ":bob!u@h PRIVMSG #history :stored",
                                    )
                                else:
                                    send("@draft/chathistory-end :fixture BATCH +h chathistory #history")
                                send(":fixture BATCH -h")
                            if registered == {"NICK", "USER", "END"} and not welcomed:
                                welcomed = True
                                send(":fixture 001 alice :Welcome")
                                send(
                                    ":fixture 005 alice CHANTYPES=# CHATHISTORY=2 "
                                    "MSGREFTYPES=msgid,timestamp :supported",
                                )
                                send(":fixture 376 alice :End of MOTD")
                    assert requested, "No history request received"
                    if iteration == 0:
                        cursor_file = Path(home) / "irc_history.conf"
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            if cursor_file.exists() and "2026-10-13T10:00:02.000Z 62" in cursor_file.read_text(
                                encoding="utf-8",
                            ):
                                break
                            time.sleep(0.02)
                        else:
                            message = "Completed cursor was not persisted"
                            raise AssertionError(message)
                        # Simulate scale-to-zero without graceful shutdown.
                        process.kill()
                    else:
                        process.terminate()
                    process.wait(timeout=5)
                log.close()
            print("PASS: real CAP negotiation, bounded LATEST, SIGKILL, durable AFTER on restart")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "build-history")
