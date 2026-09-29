# Launches and closes editors through a machine-wide registry so several agents can share one machine.
# Every editor gets an index. Whoever launched an editor is the only one allowed to close it.
# Usage:
#   python editor_session.py launch --owner <name> [--uproject <path>] [-- <extra editor args>]
#   python editor_session.py close --owner <name> <index>
#   python editor_session.py release --owner <name> [--uproject <path>]
#   python editor_session.py list

import json
import os
import subprocess
import sys
import time
from pathlib import Path

REGISTRY_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "UAssetWorkbench"
REGISTRY_PATH = REGISTRY_DIR / "editor_sessions.json"
LOCK_PATH = REGISTRY_DIR / "editor_sessions.lock"
LOCK_TIMEOUT_SEC = 10.0

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_REFUSED = 3


def default_uproject():
    project_dir = Path(__file__).resolve().parents[3]
    found = sorted(project_dir.glob("*.uproject"))
    return found[0] if found else None


def normalize_dir(path):
    return os.path.normcase(os.path.normpath(str(path)))


class RegistryLock:
    def __enter__(self):
        REGISTRY_DIR.mkdir(parents=True, exist_ok=True)
        deadline = time.time() + LOCK_TIMEOUT_SEC
        while True:
            try:
                self.fd = os.open(str(LOCK_PATH), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                return self
            except FileExistsError:
                # A lock older than the timeout was left by a crashed run.
                stale = time.time() - LOCK_PATH.stat().st_mtime > LOCK_TIMEOUT_SEC
                if stale or time.time() > deadline:
                    LOCK_PATH.unlink(missing_ok=True)
                    continue
                time.sleep(0.1)

    def __exit__(self, *args):
        os.close(self.fd)
        LOCK_PATH.unlink(missing_ok=True)


def is_editor_alive(pid):
    out = subprocess.run(["tasklist", "/FI", "PID eq {}".format(pid), "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return "UnrealEditor" in out


def load_live_sessions():
    try:
        sessions = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError):
        sessions = []
    return [s for s in sessions if is_editor_alive(s["pid"])]


def save_sessions(sessions):
    REGISTRY_PATH.write_text(json.dumps(sessions, indent=2), encoding="utf-8")


def engine_editor_exe(uproject):
    association = json.loads(Path(uproject).read_text(encoding="utf-8"))["EngineAssociation"]
    import winreg
    key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine\{}".format(association))
    engine_dir = winreg.QueryValueEx(key, "InstalledDirectory")[0]
    return Path(engine_dir) / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"


def describe(session):
    return "EDITOR #{} pid={} owner={} project={}".format(session["index"], session["pid"], session["owner"], session["uproject"])


def cmd_launch(owner, uproject, extra_args):
    uproject = Path(uproject).resolve()
    project_dir = uproject.parent
    with RegistryLock():
        sessions = load_live_sessions()
        used = {s["index"] for s in sessions}
        index = next(i for i in range(1, len(used) + 2) if i not in used)

        # A heartbeat left by a killed editor makes the task queue look online. Only clear it when
        # no live editor of this project owns it.
        same_project = [s for s in sessions if normalize_dir(Path(s["uproject"]).parent) == normalize_dir(project_dir)]
        if not same_project:
            (project_dir / "Saved" / "UAssetWorkbenchTaskQueue" / ".alive").unlink(missing_ok=True)

        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        proc = subprocess.Popen([str(engine_editor_exe(uproject)), str(uproject)] + extra_args, creationflags=flags, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        session = {"index": index, "pid": proc.pid, "owner": owner, "uproject": str(uproject), "started": time.strftime("%Y-%m-%d %H:%M:%S")}
        sessions.append(session)
        save_sessions(sessions)

    print(describe(session))
    for other in same_project:
        print("NOTE: {} also runs this project, pass --editor {} to exec_in_editor.py".format(describe(other), index))
    return EXIT_OK


def cmd_close(owner, index):
    with RegistryLock():
        sessions = load_live_sessions()
        session = next((s for s in sessions if s["index"] == index), None)
        if session is None:
            print("REFUSED: no live editor #{} in the registry".format(index))
            save_sessions(sessions)
            return EXIT_REFUSED
        if session["owner"] != owner:
            print("REFUSED: {} belongs to '{}', ask that agent to close it".format(describe(session), session["owner"]))
            return EXIT_REFUSED

        # /T takes the editor's own children (shader workers, live coding console) down with it.
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(session["pid"])], capture_output=True)
        sessions.remove(session)
        save_sessions(sessions)

    print("CLOSED {}".format(describe(session)))
    return EXIT_OK


def cmd_release(owner, uproject):
    project_dir = normalize_dir(Path(uproject).resolve().parent)
    with RegistryLock():
        sessions = load_live_sessions()
        on_project = [s for s in sessions if normalize_dir(Path(s["uproject"]).parent) == project_dir]
        mine = [s for s in on_project if s["owner"] == owner]
        for session in mine:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(session["pid"])], capture_output=True)
            sessions.remove(session)
            print("CLOSED {}".format(describe(session)))
        save_sessions(sessions)

    # A build links into Binaries of this project, any other editor still holding it fails the build.
    others = [s for s in on_project if s["owner"] != owner]
    for session in others:
        print("BLOCKED by {}, ask that agent to close it".format(describe(session)))
    return EXIT_REFUSED if others else EXIT_OK


def cmd_list():
    with RegistryLock():
        sessions = load_live_sessions()
        save_sessions(sessions)
    for session in sessions:
        print(describe(session))
    if not sessions:
        print("no registered editor is running")
    return EXIT_OK


def find_session(index):
    with RegistryLock():
        return next((s for s in load_live_sessions() if s["index"] == index), None)


def main(argv):
    if not argv:
        print("usage: editor_session.py launch --owner <name> | close --owner <name> <index> | release --owner <name> | list")
        return EXIT_USAGE

    command, rest = argv[0], argv[1:]
    extra_args = []
    if "--" in rest:
        split = rest.index("--")
        rest, extra_args = rest[:split], rest[split + 1:]

    def take(flag):
        if flag in rest:
            i = rest.index(flag)
            value = rest[i + 1]
            del rest[i:i + 2]
            return value
        return None

    owner = take("--owner")
    uproject = take("--uproject") or default_uproject()

    if command == "list":
        return cmd_list()
    if not owner:
        print("usage: --owner <name> is required, it is who may close the editor later")
        return EXIT_USAGE
    if command == "launch":
        return cmd_launch(owner, uproject, extra_args)
    if command == "close" and len(rest) == 1 and rest[0].isdigit():
        return cmd_close(owner, int(rest[0]))
    if command == "release":
        return cmd_release(owner, uproject)

    print("usage: editor_session.py launch --owner <name> | close --owner <name> <index> | release --owner <name> | list")
    return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
