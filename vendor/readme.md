# vendor

Single-header decoders from [mackron/dr_libs](https://github.com/mackron/dr_libs), public domain (Unlicense) or MIT-0, at commit `35b5c392cc15456736c440a68dbe073178bbffe0`:

- `dr_mp3.h` v0.7.4: `read-mp3`
- `dr_flac.h` v0.13.4: `read-flac`
- `dr_wav.h` v0.14.6: `read-wav`

From [schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont) at commit `853a0a171759f1ddba0de1442133a75912bbeffa`, for `render-midi`:

- `tsf.h` v0.9 (MIT): SoundFont 2 synthesizer, with `tsf.patch` applied. The patch checks that every index in a font's preset tables stays inside its table before tsf follows it (a font with a zero-size chunk read outside them), and replaces a left shift of a negative value. Upstream tsf.h, then `patch vendor/tsf.h < vendor/tsf.patch`, gives this file; the file keeps upstream's CRLF line ends.

TinySoundFont's MIDI loader, `tml.h`, is not vendored: it dropped gaps of 2^20 ticks and cut tracks without a word. `examples/lib/midi.slap` reads MIDI files instead.

slap.c defines `DR_MP3_IMPLEMENTATION`, `DR_FLAC_IMPLEMENTATION` and `DR_WAV_IMPLEMENTATION` before it includes them, and `TSF_IMPLEMENTATION` likewise. To update, replace each file whole:

```bash
C=<commit>; for f in dr_mp3 dr_flac dr_wav; do curl -sfL -o vendor/$f.h https://raw.githubusercontent.com/mackron/dr_libs/$C/$f.h; done
C=<commit>; curl -sfL -o vendor/tsf.h https://raw.githubusercontent.com/schellingb/TinySoundFont/$C/tsf.h && patch vendor/tsf.h < vendor/tsf.patch
```
