# Discord YouTube Music Bot — native Ubuntu edition

This branch contains the C++20 Ubuntu rewrite. Media downloading, WebM parsing,
buffering, Opus processing, and Discord voice transport run natively in one
process without FFmpeg or GStreamer.

See [cpp/README.md](cpp/README.md) for architecture, configuration, `.deb`
packaging, installation, systemd setup, and commands.

Quick start on Ubuntu:

```bash
chmod +x scripts/build-deb.sh
./scripts/build-deb.sh --service
sudoedit /etc/discord-music-bot/config.env
sudo systemctl start discord-music-bot
```
