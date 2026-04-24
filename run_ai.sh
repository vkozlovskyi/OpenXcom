#!/usr/bin/env bash
# Launch OpenXcom with the AI Bridge TCP server enabled.
# Extra args are forwarded to the openxcom binary.

set -eu

PORT=12345
ROOT="$(cd "$(dirname "$0")" && pwd)"

EXE=""
for candidate in \
    "$ROOT/build/openxcom.app/Contents/MacOS/openxcom" \
    "$ROOT/build/bin/openxcom" \
    "$ROOT/build/bin/Release/openxcom" \
    "$ROOT/build/bin/Debug/openxcom" \
    "$ROOT/bin/openxcom" \
    "$ROOT/openxcom"; do
  if [ -x "$candidate" ]; then
    EXE="$candidate"
    break
  fi
done

if [ -z "$EXE" ] && command -v openxcom >/dev/null 2>&1; then
  EXE="openxcom"
fi

if [ -z "$EXE" ]; then
  echo "ERROR: openxcom binary not found." >&2
  echo "  Searched build/bin/{,Release,Debug}/openxcom, bin/openxcom, ./openxcom, and PATH." >&2
  echo "  Build the game first (see README), or edit EXE= in this script." >&2
  exit 1
fi

# CD into the exe's directory so OpenXcom finds standard/ and UFO/ data folders.
cd "$(dirname "$EXE")"
exec "$EXE" -ai-server "$PORT" "$@"
