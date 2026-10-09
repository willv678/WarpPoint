#!/usr/bin/env bash
# End-to-end test: the real `warppoint` host and `warppoint-agent` processes on this machine,
# finding each other through discovery. Usage: scripts/e2e_test.sh [build-dir]
set -euo pipefail

BUILD="$(cd "${1:-build}" && pwd)"
HOST="$BUILD/host/warppoint"
AGENT="$BUILD/agent/warppoint-agent"
WORK="$(mktemp -d)"
AGENT_PID=""
HOST_PID=""

cleanup() {
  [[ -n "$HOST_PID" ]] && kill "$HOST_PID" 2>/dev/null || true
  [[ -n "$AGENT_PID" ]] && kill "$AGENT_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*"
  echo "--- host log";  cat "$WORK/host.log" 2>/dev/null || true
  echo "--- agent log"; cat "$WORK/agent.log" 2>/dev/null || true
  exit 1
}

# Wait up to 15 s for file $1 to contain exactly $2.
expect_file() {
  for _ in $(seq 1 150); do
    [[ -f "$1" && "$(cat "$1")" == "$2" ]] && return 0
    sleep 0.1
  done
  fail "expected '$2' in $1, got '$(cat "$1" 2>/dev/null || echo '<missing>')'"
}

start_agent() {
  "$AGENT" --root "$WORK/sd" --name "E2E Switch" >>"$WORK/agent.log" 2>&1 &
  AGENT_PID=$!
}

stop_agent() {
  kill "$AGENT_PID"
  wait "$AGENT_PID" 2>/dev/null || true
  AGENT_PID=""
}

mkdir -p "$WORK/mac" "$WORK/sd/emu/saves" "$WORK/state"
cat >"$WORK/state/config.toml" <<CONF
interval_seconds = 1
[unit.emu]
host = "$WORK/mac"
switch = "sdmc:/emu/saves"
CONF

echo "1. First pairing pushes into an empty Switch folder"
echo "mac v1" >"$WORK/mac/game.srm"
start_agent
"$HOST" run --state "$WORK/state" >"$WORK/host.log" 2>&1 &
HOST_PID=$!
expect_file "$WORK/sd/emu/saves/game.srm" "mac v1"

echo "2. Switch edits come back to the Mac"
echo "switch v2" >"$WORK/sd/emu/saves/game.srm"
expect_file "$WORK/mac/game.srm" "switch v2"

echo "3. Edits made while the Switch is off are synced after it returns"
stop_agent
echo "mac v3 (offline)" >"$WORK/mac/game.srm"
echo "switch-only file" >"$WORK/sd/emu/saves/other.srm"
sleep 1
start_agent
# Both sides changed while apart: that is a conflict, and nothing is overwritten.
for _ in $(seq 1 150); do
  grep -q "CONFLICT" "$WORK/host.log" && break
  sleep 0.1
done
grep -q "CONFLICT" "$WORK/host.log" || fail "expected a conflict"
[[ "$(cat "$WORK/mac/game.srm")" == "mac v3 (offline)" ]] || fail "mac copy was overwritten"
[[ "$(cat "$WORK/sd/emu/saves/game.srm")" == "switch v2" ]] || fail "switch copy was overwritten"

echo "4. Resolving keeps the chosen side and backs up the other"
kill "$HOST_PID"; wait "$HOST_PID" 2>/dev/null || true; HOST_PID=""
"$HOST" resolve emu host --state "$WORK/state" >/dev/null
"$HOST" run --once --state "$WORK/state" >>"$WORK/host.log" 2>&1 || fail "run --once failed"
expect_file "$WORK/sd/emu/saves/game.srm" "mac v3 (offline)"
[[ ! -f "$WORK/sd/emu/saves/other.srm" ]] || fail "switch-only file should be replaced by the host unit"
ls "$WORK/sd/.warppoint-agent/backups/dir_emu"/*/other.srm >/dev/null 2>&1 ||
  fail "switch copy was not backed up"
ls "$WORK/state/conflicts/dir_emu"/*-switch/game.srm >/dev/null 2>&1 ||
  fail "conflict snapshot missing"

echo "5. Agent stops cleanly on SIGTERM while a host is connected"
"$HOST" run --state "$WORK/state" >>"$WORK/host.log" 2>&1 &
HOST_PID=$!
sleep 2
kill "$AGENT_PID"
for _ in $(seq 1 50); do
  kill -0 "$AGENT_PID" 2>/dev/null || break
  sleep 0.1
done
kill -0 "$AGENT_PID" 2>/dev/null && fail "agent did not stop"
AGENT_PID=""

echo "PASS"
