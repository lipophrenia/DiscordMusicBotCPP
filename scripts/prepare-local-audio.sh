#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 INPUT_AUDIO OUTPUT.webm" >&2
    exit 2
fi
if [ ! -f "$1" ]; then
    echo "Input audio file not found." >&2
    exit 2
fi
case "$2" in
    *.webm) ;;
    *) echo "Output filename must end with .webm" >&2; exit 2 ;;
esac
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "FFmpeg is required only for offline conversion." >&2
    exit 2
fi

# file: prevents filenames from being interpreted as options or network URLs.
# -n preserves an existing recording rather than overwriting it.
exec ffmpeg -hide_banner -nostdin -n -i "file:$1" -map 0:a:0 -vn \
    -c:a libopus -ar 48000 -ac 2 -b:a 128k -frame_duration 20 \
    -f webm "file:$2"
