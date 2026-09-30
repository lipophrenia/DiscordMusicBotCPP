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

Playback starts after `PREBUFFER_SECONDS` (2 seconds by default). The download
cursor may then lead playback by `BUFFER_AHEAD_SECONDS` (60 seconds by default).
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
| `MAX_TRACK_DURATION` | `10800` | Track limit in seconds; 0 disables it |
| `IDLE_TIMEOUT` | `300` | Disconnect timeout, minimum 30 seconds |
| `BUFFER_AHEAD_SECONDS` | `60` | Download lead, 5–3600 seconds |
| `PREBUFFER_SECONDS` | `2` | Initial in-process Opus buffer |
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
`/leave`, `/queue`, `/now`, and `/volume`. There is no `/music` prefix.

## Manual development build

```bash
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```
