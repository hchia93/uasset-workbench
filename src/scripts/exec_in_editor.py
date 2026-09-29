# Executes a python file (or statement) inside a running editor via the engine's
# python remote-execution protocol: multicast discovery, then the editor dials back
# over TCP and runs the command in-process.
# Only editors of this script's own project answer. With several of them up, --editor
# picks one by its editor_session.py index.
# Usage:
#   python exec_in_editor.py [--editor <index>] <script.py>
#   python exec_in_editor.py [--editor <index>] --cmd "unreal.log('hi')"

import json
import os
import socket
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import editor_session

MULTICAST_GROUP = ("239.0.0.1", 6766)
LOCAL_ADAPTER = "127.0.0.1"
MAGIC = "ue_py"
VERSION = 1
NODE_ID = str(uuid.uuid4())
DISCOVER_TIMEOUT_SEC = 3.0
RESULT_TIMEOUT_SEC = 120.0
PID_PROBE = "import os; print('UAW_PID=' + str(os.getpid()))"


def message(msg_type, data=None, dest=None):
    payload = {"version": VERSION, "magic": MAGIC, "type": msg_type, "source": NODE_ID}
    if dest is not None:
        payload["dest"] = dest
    if data is not None:
        payload["data"] = data
    return json.dumps(payload).encode("utf-8")


def parse(blob):
    try:
        payload = json.loads(blob.decode("utf-8"))
    except ValueError:
        return None
    if payload.get("magic") != MAGIC or payload.get("source") == NODE_ID:
        return None
    return payload


def discover_all(udp):
    udp.sendto(message("ping"), MULTICAST_GROUP)
    nodes = {}
    deadline = time.time() + DISCOVER_TIMEOUT_SEC
    while time.time() < deadline:
        udp.settimeout(max(0.1, deadline - time.time()))
        try:
            blob, _ = udp.recvfrom(8192)
        except socket.timeout:
            break
        payload = parse(blob)
        if payload and payload.get("type") == "pong":
            nodes[payload["source"]] = payload.get("data") or {}
    return nodes


def run_on_node(udp, node, command, exec_mode):
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind((LOCAL_ADAPTER, 0))
    tcp_server.listen(1)
    command_port = tcp_server.getsockname()[1]

    udp.sendto(message("open_connection", {"command_ip": LOCAL_ADAPTER, "command_port": command_port}, dest=node), MULTICAST_GROUP)

    tcp_server.settimeout(DISCOVER_TIMEOUT_SEC)
    try:
        conn, _ = tcp_server.accept()
    except socket.timeout:
        tcp_server.close()
        return None, "editor did not dial back"

    conn.sendall(message("command", {"command": command, "unattended": True, "exec_mode": exec_mode}, dest=node))

    conn.settimeout(RESULT_TIMEOUT_SEC)
    buffer = b""
    result = None
    error = None
    while result is None:
        try:
            chunk = conn.recv(8192)
        except socket.timeout:
            error = "timed out waiting for command_result"
            break
        if not chunk:
            break
        buffer += chunk
        payload = parse(buffer)
        if payload and payload.get("type") == "command_result":
            result = payload["data"]

    udp.sendto(message("close_connection", dest=node), MULTICAST_GROUP)
    conn.close()
    tcp_server.close()
    if result is None and error is None:
        error = "connection closed without a result"
    return result, error


def probe_pid(udp, node):
    result, _ = run_on_node(udp, node, PID_PROBE, "ExecuteStatement")
    for entry in (result or {}).get("output", []):
        text = entry.get("output", "")
        if "UAW_PID=" in text:
            return int(text.split("UAW_PID=")[1].split()[0])
    return None


def matches_project(data, project_dir):
    root = data.get("project_root")
    return not root or editor_session.normalize_dir(root) == editor_session.normalize_dir(project_dir)


def main():
    args = sys.argv[1:]
    editor_index = None
    if len(args) >= 2 and args[0] == "--editor":
        editor_index, args = int(args[1]), args[2:]

    if not args:
        print("usage: exec_in_editor.py [--editor <index>] <script.py> | --cmd <statement>")
        return 2

    if args[0] == "--cmd":
        command, exec_mode = args[1], "ExecuteStatement"
    else:
        command, exec_mode = args[0], "ExecuteFile"

    session = None
    if editor_index is not None:
        session = editor_session.find_session(editor_index)
        if session is None:
            print("FAIL: no live editor #{} in the registry, see editor_session.py list".format(editor_index))
            return 1
        project_dir = Path(session["uproject"]).parent
    else:
        project_dir = Path(__file__).resolve().parents[3]

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("0.0.0.0", MULTICAST_GROUP[1]))
    membership = socket.inet_aton(MULTICAST_GROUP[0]) + socket.inet_aton(LOCAL_ADAPTER)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)

    nodes = discover_all(udp)
    candidates = [node for node, data in nodes.items() if matches_project(data, project_dir)]
    if not candidates:
        print("FAIL: no editor of {} answered the ping (editor closed, or remote execution disabled)".format(project_dir))
        return 1

    if session is not None:
        target = next((node for node in candidates if probe_pid(udp, node) == session["pid"]), None)
        if target is None:
            print("FAIL: editor #{} (pid {}) did not answer yet".format(editor_index, session["pid"]))
            return 1
    elif len(candidates) > 1:
        print("FAIL: {} editors of {} are up, pick one with --editor <index>:".format(len(candidates), project_dir))
        editor_session.cmd_list()
        return 1
    else:
        target = candidates[0]

    result, error = run_on_node(udp, target, command, exec_mode)
    udp.close()

    if result is None:
        print("FAIL: {}".format(error))
        return 1

    for entry in result.get("output", []):
        print("[{}] {}".format(entry.get("type"), entry.get("output", "").rstrip()))
    if not result.get("success"):
        print("FAIL: {}".format(result.get("result")))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
