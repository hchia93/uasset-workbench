#!/usr/bin/env bash
#
# Wrapper for UAssetWorkbench.
#
# Routing:
#   1. If <ProjectDir>/Saved/UAssetWorkbenchTaskQueue/.alive is fresh (mtime within
#      HEARTBEAT_FRESHNESS_SEC, or the pid it holds still running), an in-editor
#      queue subsystem is active. Write a pending task json, wait for the
#      corresponding done/<uuid>.json.
#   2. Otherwise, launch the commandlet directly (legacy path). The commandlet
#      itself also checks the heartbeat and aborts on conflict.
#
# Usage:
#   run_commandlet.sh <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]
#
# Args:
#   UE_PATH     Engine install root, e.g. "C:/Program Files/Epic Games/UE_5.7"
#   UPROJECT    Absolute path to the .uproject file
#   RunName     Commandlet name, e.g. "BlueprintEdGraphExport"
#   AssetList   Comma-separated asset paths, e.g. "/Game/Foo/BP_A,/Game/Bar/BP_B"
#               Spec-driven runs (Edit*, *Import, Create*) name their targets in the spec, pass ""
#   IDLE_SEC    Export runs: seconds of output mtime stability before kill. Default 10.
#   MAX_SEC     Absolute upper bound on total wait. Default 600.
#   EXTRA_ARGS  Forwarded to the commandlet, e.g. "-graphs" or "-spec=C:/path/spec.json". Default empty.
#
# Run groups (same rule as the C++ side):
#   *Export  produce JSON under Intermediate/UAssetExport, completion = every output present and settled
#   *Import  regenerate an asset from a spec, Create* also lands here, completion = process exit code
#   Edit*    change an existing asset to match an intent, completion = process exit code
#   Audit*   read-only reports, completion = process exit code
#   others   migration runs (redirect / resave / replace), completion = process exit code
#
# Exit codes (passed straight through from the commandlet):
#   0 - success
#   1 - failure, bad args, or an expected output is missing
#   2 - bad invocation, or the editor is running and the run refused to fight it for the project lock
#   3 - audit runs only: the run itself succeeded and the report lists something to fix

set -u

HEARTBEAT_FRESHNESS_SEC=15

if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]" >&2
    exit 2
fi

UE_PATH="$1"
UPROJECT="$2"
RUN="$3"
ASSETS="$4"
IDLE_SEC="${5:-10}"
MAX_SEC="${6:-600}"
EXTRA_ARGS="${7:-}"

case "$RUN" in
    *Export) GROUP="export" ;;
    *Import) GROUP="import" ;;
    Create*) GROUP="import" ;;
    Edit*)   GROUP="edit" ;;
    Audit*)  GROUP="audit" ;;
    *)       GROUP="migrate" ;;
esac

UE_CMD="$UE_PATH/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
PROJECT_DIR="$(dirname "$UPROJECT")"
EXPORT_ROOT="$PROJECT_DIR/Intermediate/UAssetExport"
QUEUE_ROOT="$PROJECT_DIR/Saved/UAssetWorkbenchTaskQueue"
ALIVE_FILE="$QUEUE_ROOT/.alive"
PENDING_DIR="$QUEUE_ROOT/pending"
DONE_DIR="$QUEUE_ROOT/done"

if [ ! -f "$UPROJECT" ]; then
    echo "[run_commandlet] not found: $UPROJECT" >&2
    exit 2
fi

# Values go inside a json string literal. An unescaped quote or backslash writes a broken
# task file, which the subsystem rejects with "Failed to parse pending task json".
json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '%s' "$s"
}

get_mtime() {
    stat -c %Y "$1" 2>/dev/null || echo 0
}

is_heartbeat_fresh() {
    if [ ! -f "$ALIVE_FILE" ]; then
        return 1
    fi
    local mtime now pid
    mtime=$(get_mtime "$ALIVE_FILE")
    now=$(date +%s)
    if [ $((now - mtime)) -le "$HEARTBEAT_FRESHNESS_SEC" ]; then
        return 0
    fi
    # The ticker that stamps the file stops during a synchronous editor stall, registry scan or shader
    # compile, so a stale mtime is not proof of death. The pid the editor wrote there is.
    pid=$(tr -dc '0-9' < "$ALIVE_FILE" 2>/dev/null)
    if is_winpid_alive "$pid"; then
        return 0
    fi
    return 1
}

# $1 = Windows pid. 0 if a live Windows process with that pid exists.
is_winpid_alive() {
    local wp="$1"
    [ -z "$wp" ] && return 1
    tasklist //FI "PID eq $wp" //NH 2>/dev/null | grep -qw "$wp"
}

# Kill a backgrounded native UnrealEditor-Cmd job. $! is an MSYS pid; Windows
# taskkill needs the Windows pid from /proc/<msys_pid>/winpid. Tree-kill (//T)
# also reaps ShaderCompileWorker / CrashReportClient children, then verify the
# Windows process is actually gone. Returns 0 only if confirmed dead.
terminate_commandlet() {
    local msys_pid="$1" win_pid="$2"

    if [ -z "$win_pid" ] && [ -r "/proc/$msys_pid/winpid" ]; then
        win_pid="$(cat "/proc/$msys_pid/winpid" 2>/dev/null)"
    fi

    if [ -n "$win_pid" ]; then
        taskkill //F //T //PID "$win_pid" >/dev/null 2>&1
    fi
    kill -9 "$msys_pid" 2>/dev/null

    local tries=0
    while [ -n "$win_pid" ] && is_winpid_alive "$win_pid"; do
        tries=$((tries + 1))
        if [ "$tries" -gt 5 ]; then
            echo "[run_commandlet] WARN WINPID=$win_pid still alive after taskkill" >&2
            return 1
        fi
        taskkill //F //T //PID "$win_pid" >/dev/null 2>&1
        sleep 1
    done
    return 0
}

# Exports are stamped with revision and capture time, so the name is not known up front.
# Only a file written by this run counts, an older capture must not be mistaken for fresh output.
newest_export_since() {
    local prefix="$1" since="$2" f best=""
    for f in "$prefix"_r*.json; do
        [ -f "$f" ] || continue
        if [ "$(get_mtime "$f")" -ge "$since" ]; then
            best="$f"
        fi
    done
    if [ -n "$best" ]; then
        echo "$best"
    fi
}

route_queue() {
    mkdir -p "$PENDING_DIR" "$DONE_DIR"

    local uuid="$(date +%s)-$$-$RANDOM"
    local pending_path="$PENDING_DIR/$uuid.json"
    local pending_tmp="$PENDING_DIR/.$uuid.json.tmp"
    local done_path="$DONE_DIR/$uuid.json"

    local assets_json=""
    local A
    IFS=',' read -r -a ASSET_ARR <<< "$ASSETS"
    for A in "${ASSET_ARR[@]}"; do
        # Splitting an empty list yields one empty entry, which the subsystem would count as a missing output.
        [ -z "$A" ] && continue
        if [ -n "$assets_json" ]; then
            assets_json="$assets_json,"
        fi
        assets_json="$assets_json\"$(json_escape "$A")\""
    done

    cat > "$pending_tmp" <<EOF
{
  "RunName": "$(json_escape "$RUN")",
  "Assets": [$assets_json],
  "ExtraArgs": "$(json_escape "$EXTRA_ARGS")"
}
EOF
    mv "$pending_tmp" "$pending_path"

    echo "[run_commandlet] queued task $uuid via in-editor subsystem" >&2

    local start_ts now
    start_ts=$(date +%s)
    while true; do
        if [ -f "$done_path" ]; then
            break
        fi
        now=$(date +%s)
        if [ $((now - start_ts)) -gt "$MAX_SEC" ]; then
            echo "[run_commandlet] queue task $uuid timed out after ${MAX_SEC}s" >&2
            return 1
        fi
        if ! is_heartbeat_fresh; then
            # The done file may have landed in the same second the editor went away.
            if [ -f "$done_path" ]; then
                break
            fi
            echo "[run_commandlet] heartbeat went stale while waiting on $uuid" >&2
            return 1
        fi
        sleep 1
    done

    local exit_code
    exit_code=$(grep -o '"ExitCode"[[:space:]]*:[[:space:]]*-\{0,1\}[0-9]\+' "$done_path" | grep -o '\-\{0,1\}[0-9]\+$')
    echo "[run_commandlet] task $uuid done exit=${exit_code:-1}" >&2
    rm -f "$done_path"
    return "${exit_code:-1}"
}

route_commandlet() {
    if [ ! -x "$UE_CMD" ] && [ ! -f "$UE_CMD" ]; then
        echo "[run_commandlet] not found: $UE_CMD" >&2
        return 2
    fi

    local A REL
    EXPECTED_PREFIXES=()
    if [ "$GROUP" = "export" ]; then
        if [ -z "$ASSETS" ]; then
            # An export with no asset list (PCGCatalogExport) names its output after the run.
            EXPECTED_PREFIXES+=("$EXPORT_ROOT/$RUN")
        else
            IFS=',' read -r -a ASSET_ARR <<< "$ASSETS"
            for A in "${ASSET_ARR[@]}"; do
                REL="${A#/}"
                EXPECTED_PREFIXES+=("$EXPORT_ROOT/$REL")
            done
        fi
    fi

    local CMD_LOG="$QUEUE_ROOT/last_commandlet.log"
    mkdir -p "$QUEUE_ROOT"
    : > "$CMD_LOG"

    # An empty -assets= makes FParse::Value swallow whatever flag follows it, so omit the flag entirely.
    local ASSETS_ARG=""
    if [ -n "$ASSETS" ]; then
        ASSETS_ARG="-assets=$ASSETS"
    fi

    MSYS_NO_PATHCONV=1 "$UE_CMD" "$UPROJECT" \
        -run="$RUN" $ASSETS_ARG $EXTRA_ARGS \
        -nullrhi -nosplash -nosound -unattended -stdout \
        >"$CMD_LOG" 2>&1 &
    local PID=$!
    local WINPID=""
    if [ -r "/proc/$PID/winpid" ]; then
        WINPID="$(cat "/proc/$PID/winpid" 2>/dev/null)"
    fi
    echo "[run_commandlet] launched PID=$PID WINPID=${WINPID:-?} run=$RUN idle=${IDLE_SEC}s max=${MAX_SEC}s" >&2

    local START_TS=$(date +%s)
    local STABLE_SINCE=0
    local LAST_SIG=""
    local KILL_REASON=""
    local NOW SIG ALL_PRESENT F P
    local EXIT_RC=0

    while kill -0 "$PID" 2>/dev/null; do
        NOW=$(date +%s)

        if [ $((NOW - START_TS)) -gt "$MAX_SEC" ]; then
            KILL_REASON="MAX_SEC ${MAX_SEC}s exceeded"
            break
        fi

        # Mutating runs leave no output file to watch, they are done when the process is done.
        if [ "$GROUP" != "export" ]; then
            sleep 1
            continue
        fi

        SIG=""
        ALL_PRESENT=1
        for P in "${EXPECTED_PREFIXES[@]}"; do
            F="$(newest_export_since "$P" "$START_TS")"
            if [ -z "$F" ]; then
                ALL_PRESENT=0
                break
            fi
            SIG="$SIG $(get_mtime "$F")"
        done

        if [ "$ALL_PRESENT" -eq 1 ]; then
            if [ "$SIG" = "$LAST_SIG" ] && [ "$STABLE_SINCE" -ne 0 ]; then
                if [ $((NOW - STABLE_SINCE)) -ge "$IDLE_SEC" ]; then
                    KILL_REASON="outputs stable for ${IDLE_SEC}s"
                    break
                fi
            else
                LAST_SIG="$SIG"
                STABLE_SINCE="$NOW"
            fi
        else
            STABLE_SINCE=0
            LAST_SIG=""
        fi

        sleep 1
    done

    if kill -0 "$PID" 2>/dev/null; then
        echo "[run_commandlet] killing PID=$PID WINPID=${WINPID:-?} reason=$KILL_REASON" >&2
        if terminate_commandlet "$PID" "$WINPID"; then
            wait "$PID" 2>/dev/null
        fi
        # A kill means the run never reported for itself, only export can call that a success.
        if [ "$GROUP" != "export" ]; then
            EXIT_RC=1
        fi
    else
        wait "$PID" 2>/dev/null
        EXIT_RC=$?
        echo "[run_commandlet] PID=$PID exited on its own rc=$EXIT_RC" >&2
    fi

    local RC=0
    if [ "$GROUP" = "export" ]; then
        for P in "${EXPECTED_PREFIXES[@]}"; do
            F="$(newest_export_since "$P" "$START_TS")"
            if [ -z "$F" ]; then
                echo "[run_commandlet] missing output: ${P}_r*.json" >&2
                RC=1
            else
                echo "[run_commandlet] output: $F" >&2
            fi
        done
    else
        RC="$EXIT_RC"
    fi
    if [ "$RC" -ne 0 ]; then
        echo "[run_commandlet] commandlet diagnostics (tail of $CMD_LOG):" >&2
        tail -n 40 "$CMD_LOG" >&2 2>/dev/null
        echo "[run_commandlet] full UE log dir: $PROJECT_DIR/Saved/Logs/" >&2
    fi
    return "$RC"
}

if is_heartbeat_fresh; then
    route_queue
    exit $?
else
    route_commandlet
    exit $?
fi
