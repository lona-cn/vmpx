#!/bin/sh
set -eu

: "${VMPX_HOME:=/opt/vmpx}"
: "${VMPX_PORT:=11451}"
: "${VMPX_SERVER_EXE:=${VMPX_HOME}/vmpx_server.exe}"
: "${VMProtect_CON:=${VMPX_HOME}/VMProtect_Con.exe}"
: "${WINEPREFIX:=${VMPX_HOME}/data/.wine}"
export VMPX_HOME VMPX_PORT VMPX_SERVER_EXE VMProtect_CON WINEPREFIX

if [ ! -f "$VMPX_SERVER_EXE" ]; then
    echo "Missing VMPX server executable: $VMPX_SERVER_EXE" >&2
    exit 2
fi
if [ ! -f "$VMPX_HOME/config/log4cplus.properties" ] || [ ! -d "$VMPX_HOME/assets/static" ]; then
    echo "VMPX_HOME must contain the extracted Windows release directory" >&2
    exit 2
fi

mkdir -p "$VMPX_HOME/data" "$WINEPREFIX"
Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp -ac &
x11vnc -display "$DISPLAY" -forever -shared -localhost -rfbport 5900 -nopw &
websockify --web=/usr/share/novnc/ 6080 localhost:5900 &
wineboot --init

set -- 0.0.0.0 "$VMPX_PORT"
if [ -f "$VMProtect_CON" ]; then
    set -- "$@" "$(winepath -w "$VMProtect_CON")"
fi
cd "$VMPX_HOME"
exec wine "$VMPX_SERVER_EXE" "$@"
