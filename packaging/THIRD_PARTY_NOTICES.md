# Third-party components

The build script downloads official release artifacts over HTTPS and verifies
the supplied SHA-256 files before packaging them.

- DPP (D++), Apache License 2.0: <https://github.com/brainboxdotcc/DPP>
- yt-dlp standalone executable: <https://github.com/yt-dlp/yt-dlp>
- Deno, MIT License: <https://github.com/denoland/deno>
- libcurl, libopus, OpenSSL, zlib and nlohmann/json are linked or included under
  their respective upstream licenses.

The official PyInstaller form of yt-dlp includes GPLv3-or-later components; see
the yt-dlp release and `--license` output for the complete notices corresponding
to the packaged executable.
