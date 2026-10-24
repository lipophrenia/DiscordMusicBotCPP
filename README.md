# Discord Music Bot

C++20 Discord music bot for YouTube and local WebM/Opus audio, with a shared
queue and volume control. YouTube URLs and metadata are resolved by `yt-dlp`
and Deno; audio playback runs natively in the bot.

## Install

On Ubuntu 22.04 or newer (amd64 or arm64), run from the repository root:

```bash
bash scripts/build-deb.sh --service
sudoedit /etc/discord-music-bot/config.env
sudo systemctl restart discord-music-bot
```

Set `DISCORD_TOKEN` before starting the service. The build script installs
build dependencies and bundles `yt-dlp` and Deno. Without options it only builds
packages into `dist/`; `--install` installs without enabling the service.
Use `BUILD_JOBS=1` on low-memory machines.

View logs:

```bash
sudo journalctl -u discord-music-bot -f
```

## Configuration

The service reads `/etc/discord-music-bot/config.env`; upgrades preserve this
file. Restart the service after changing settings. Manual runs also accept
environment variables and `.env` files in the working directory, executable
directory or `~/.config/DiscordMusicBot/`.

| Variable | Default | Purpose |
|---|---|---|
| `DISCORD_TOKEN` | required | Discord bot token |
| `DISCORD_GUILD_ID` | empty | Register commands in one guild instead of globally |
| `DEFAULT_VOLUME` | `50` | Initial volume, 0–200% |
| `MAX_TRACK_DURATION` | `10800` | YouTube duration limit in seconds; 0 disables it |
| `IDLE_TIMEOUT` | `300` | Idle disconnect timeout in seconds; minimum 30 |
| `BUFFER_AHEAD_SECONDS` | `60` | Network download lead in seconds, 5–3600 |
| `PREBUFFER_SECONDS` | `2` | Network startup buffer in seconds |
| `LOCAL_AUDIO_DIR` | `local-audio` | Directory of WebM/Opus files |
| `YOUTUBE_COOKIES_FILE` | empty | Path to a Netscape-format cookie file |
| `YT_DLP_PATH` | `yt-dlp` | YouTube extractor executable |
| `DENO_PATH` | `deno` | JavaScript runtime for the extractor |

Local files bypass both network buffer settings. If YouTube requires cookies,
copy them to a location readable by the service and set `YOUTUBE_COOKIES_FILE`:

```bash
sudo install -o root -g discord-music-bot -m 640 youtube-cookies.txt \
  /etc/discord-music-bot/youtube-cookies.txt
```

## Commands

Join a voice channel before requesting playback.

| Command | Action |
|---|---|
| `/join` | Connect the bot to your voice channel |
| `/play query:...` | Queue a YouTube URL or search result |
| `/list-local [page]` | Browse local tracks and click a song to queue it |
| `/play-local file:...` | Queue a local file by its full name, including `.webm` |
| `/pause`, `/resume` | Pause or resume playback |
| `/skip` | Skip the current track |
| `/stop` | Stop playback and clear the queue |
| `/leave` | Stop playback and disconnect |
| `/queue`, `/now` | Show the queue or current track |
| `/volume percent:...` | Set volume from 0 to 200% |

## Local audio

Place **WebM/Opus** files in `LOCAL_AUDIO_DIR`. The service's default directory
is `/var/lib/discord-music-bot/local-audio`. Only regular `.webm` files directly
inside the directory are listed; hidden files, symlinks and subdirectories are
ignored. Adding or removing recordings does not require a restart.

`/list-local` displays four song buttons per page in a single column, without file extensions.
Use **Previous / Next** to switch pages. Clicking a song adds it to the shared
queue and starts playback if idle. Buttons expire after 15 minutes or a bot
restart; run `/list-local` again to refresh them.

Convert MP3, FLAC or another audio format using FFmpeg on the conversion machine:

```bash
mkdir -p converted
sh scripts/prepare-local-audio.sh 'source.flac' 'converted/My song.webm'
sudo install -o root -g discord-music-bot -m 640 'converted/My song.webm' \
  '/var/lib/discord-music-bot/local-audio/My song.webm'
```

The script produces 48 kHz stereo Opus at 128 kbit/s with 20 ms packets and
preserves existing output files. FFmpeg is only needed for conversion. Copy
completed files into the library; the systemd service cannot access `/home`.

## Development build

With the build dependencies installed:

```bash
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

## Contact

Maxim "lipophrenia-" Ryabtsev
