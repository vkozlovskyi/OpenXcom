#!/usr/bin/env bash
# Launch OpenXcom with the AI Bridge TCP server enabled.
# Extra args are forwarded to the openxcom binary.

set -eu

PORT=12345
EXE=""
for candidate in \
    build/bin/openxcom \
    build/bin/Release/openxcom \
    build/bin/Debug/openxcom \
    bin/openxcom \
    ./openxcom; do
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

exec "$EXE" -ai-server "$PORT" "$@"
