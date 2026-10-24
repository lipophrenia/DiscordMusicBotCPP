# Discord Music Bot

A resource-conscious C++20 Discord music bot for Ubuntu. Discord voice, HTTP
media transfer, WebM demuxing, buffering and Opus volume processing all run
inside one process. FFmpeg, GStreamer and Python are not used for media.

`yt-dlp` is intentionally retained as a small compatibility helper: it resolves
YouTube searches, metadata and expiring media URLs. It never downloads or
decodes the audio stream. Reimplementing YouTube's changing signature code in
the bot would make releases fragile without reducing media CPU or memory use.

## Runtime pipeline

```text
/play -> yt-dlp metadata/URL -> libcurl -> native WebM parser -> Opus -> DPP voice
                                  |                         |
                            download cursor          playback cursor
```

Network playback starts after collecting `PREBUFFER_SECONDS` of audio (2 seconds
by default). The download cursor may then lead playback by `BUFFER_AHEAD_SECONDS`
(60 seconds by default). These two settings do not apply to local files.
Backpressure pauses libcurl, so long tracks cannot consume unbounded memory.
At 100% volume, complete WebM blocks are passed through without a bot-owned
audio-payload copy or Opus transcoding. A copy is retained only for the startup
prebuffer and for an element split across libcurl chunks. Other volume levels
necessarily decode, scale and re-encode Opus audio.

The configured YouTube cookie file is treated as a read-only source. Under the
systemd service, the bot creates a private writable copy in
`/var/cache/discord-music-bot`, because yt-dlp updates its cookie jar on exit.

## Dependencies

Build-time dependencies are installed by `scripts/build-deb.sh`. DPP is fetched
at a pinned tag and linked statically. The script also downloads the official
standalone `yt-dlp` and Deno binaries, verifies both against their official
release SHA-256 files, and embeds them in the package. Deno is required by
modern YouTube signature challenges. The target machine therefore needs
neither Python nor system `yt-dlp`/Deno packages. Shared-library dependencies
are discovered automatically.

## Build a Debian package

From the repository root on Ubuntu 22.04 or newer:

```bash
chmod +x scripts/build-deb.sh
./scripts/build-deb.sh
```

The package is written to `dist/`.
On a low-memory machine, limit compiler parallelism with `BUILD_JOBS=1`.

Build and install it:

```bash
./scripts/build-deb.sh --install
```

Build, install, and enable a systemd unit:

```bash
./scripts/build-deb.sh --service
```

If the token is still `replace_me`, the service is enabled but deliberately not
started. Configure it and start the unit:

```bash
sudoedit /etc/discord-music-bot/config.env
sudo systemctl start discord-music-bot
sudo journalctl -u discord-music-bot -f
```

The package does not remove `/etc/discord-music-bot/config.env` on upgrades.

## Configuration

The service reads `/etc/discord-music-bot/config.env`. An interactive run also
accepts environment variables, `.env` in the current/executable directory, and
`~/.config/DiscordMusicBot/.env`.

| Variable | Default | Purpose |
|---|---:|---|
| `DISCORD_TOKEN` | required | Discord bot token |
| `DISCORD_GUILD_ID` | empty | Development guild for immediate command updates |
| `DEFAULT_VOLUME` | `50` | Initial volume, 0–200% |
| `MAX_TRACK_DURATION` | `10800` | YouTube track limit in seconds; 0 disables it |
| `IDLE_TIMEOUT` | `300` | Disconnect timeout, minimum 30 seconds |
| `BUFFER_AHEAD_SECONDS` | `60` | Network download lead, 5–3600 seconds |
| `PREBUFFER_SECONDS` | `2` | Network startup Opus buffer; ignored for local files |
| `LOCAL_AUDIO_DIR` | `local-audio` | Directory containing preconverted WebM/Opus recordings |
| `YOUTUBE_COOKIES_FILE` | empty | Absolute/relative Netscape cookie file |
| `YT_DLP_PATH` | `yt-dlp` | Extractor executable |
| `DENO_PATH` | `deno` | JavaScript runtime used only by the extractor |

For the system service, store cookies outside Git and allow the service user to
read them:

```bash
sudo install -o root -g discord-music-bot -m 640 youtube-cookies.txt \
  /etc/discord-music-bot/youtube-cookies.txt
```

Then set:

```dotenv
YOUTUBE_COOKIES_FILE=/etc/discord-music-bot/youtube-cookies.txt
```

## Commands

Commands are top-level: `/join`, `/play`, `/pause`, `/resume`, `/skip`, `/stop`,
`/leave`, `/queue`, `/now`, `/volume`, `/list-local`, and `/play-local`.
There is no `/music` prefix.

## Local audio

Set `LOCAL_AUDIO_DIR` to a directory containing **WebM with Opus audio**.
Relative paths use the process working directory. The default is `local-audio`;
for the systemd service that means `/var/lib/discord-music-bot/local-audio`.
The Debian package creates that directory with access for the service user.
Only regular `.webm` files directly inside the directory are listed; subdirectories,
hidden files, symbolic links and other formats are ignored.

Commands:

```text
/list-local
/list-local page:2
/play-local file:My song.webm
```

`/list-local` shows a private, interactive list with five recordings per page.
Titles and song buttons omit the `.webm` extension. Click a song to join your
voice channel and queue that recording, starting playback if the player is idle.
Use **Назад / Далее** to browse pages in the same message; `/list-local page:2`
also works. Each page request refreshes the directory listing.

Buttons keep the exact filename that was displayed, so adding or removing other
recordings cannot change their target. Deleted files report an error. Buttons
expire after 15 minutes or a bot restart; run `/list-local` again to refresh them.
Long titles may be shortened on buttons; the full title is shown above them.
Spaces and UTF-8 names are supported.

`/play-local file:...` remains available for manual selection: `file` is the exact
filename **including** `.webm`. Both commands use the same playback queue and
voice-channel checks.

`/play-local` joins your voice channel and adds the recording to the shared
queue, starting immediately if the player is idle. YouTube and local recordings
can be mixed. `/pause`, `/resume`, `/skip`, `/stop`, `/volume`, `/queue` and `/now`
apply to both sources. Local entries have no YouTube link; duration is displayed
as unknown because the bot does not scan the entire file before queueing it.
`MAX_TRACK_DURATION` only applies to YouTube metadata.

Local playback uses the same streaming demuxer and libopus volume processing,
with **no startup prebuffer**. Each decoded WebM block is passed straight to the
voice sender. Local files are read and enqueued in DPP as fast as file I/O and
Opus processing allow, with no queue lead limit. DPP sends packets at playback
speed; the first packet does not wait for the rest of the file. `/play-local`
enqueues the track before sending its final response to Discord, without
waiting for a deferred `thinking` acknowledgement.

A newly established voice connection must still complete its Discord handshake,
including DAVE negotiation. Playback waits for DPP's `on_voice_ready` event before
reading the track or announcing playback: `is_ready()` alone only confirms the
transport key, and DPP encrypts packets when they enter its queue. This prevents
the beginning of a recording from being queued before DAVE is ready.
The log records `Voice session ready` with the DAVE status and separates
`Voice ready` wait time from `Local media first packet queued` time so a
connection delay can be distinguished from file/codec processing.
Local playback does not invoke yt-dlp, Deno, or FFmpeg. Missing YouTube
helpers produce a startup warning and do not prevent local playback. A missing
library directory is treated as an empty library; unreadable/deleted/invalid
files report an error and the player moves to the next queued recording.

### Offline conversion

Install FFmpeg on the machine doing the conversion, then run:

```bash
mkdir -p local-audio
sh scripts/prepare-local-audio.sh 'source.mp3' 'local-audio/My song.webm'
```

The script produces audio-only WebM/Opus, 48 kHz, stereo, 128 kbit/s, with 20 ms
packets. It leaves existing output files intact. FFmpeg is not a bot runtime
dependency. Convert into a staging directory and then move completed files into
the library so the bot cannot select a partially written recording.

For a packaged service, copy the prepared recording with suitable permissions:

```bash
sudo install -o root -g discord-music-bot -m 640 'local-audio/My song.webm' \
  '/var/lib/discord-music-bot/local-audio/My song.webm'
```

The service's `ProtectHome=true` prevents access to libraries under `/home`;
use its state directory or another location readable by `discord-music-bot`.
Restart the bot after changing `LOCAL_AUDIO_DIR`. Adding/removing files does not
require a restart.

## Manual development build

```bash
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```
