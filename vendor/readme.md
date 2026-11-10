# vendor

From [schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont) at commit `853a0a171759f1ddba0de1442133a75912bbeffa`, for `render-midi`:

- `tsf.h` v0.9 (MIT): SoundFont 2 synthesizer, with `tsf.patch` applied. The patch checks that every index in a font's preset tables stays inside its table before tsf follows it (a font with a zero-size chunk read outside them), and replaces a left shift of a negative value. Upstream tsf.h, then `patch vendor/tsf.h < vendor/tsf.patch`, gives this file; the file keeps upstream's CRLF line ends.

TinySoundFont's MIDI loader, `tml.h`, is not vendored: it dropped gaps of 2^20 ticks and cut tracks without a word. `examples/lib/midi.slap` reads MIDI files instead.

slap.c defines `TSF_IMPLEMENTATION` before it includes tsf.h. To update, replace the file whole:

```bash
C=<commit>; curl -sfL -o vendor/tsf.h https://raw.githubusercontent.com/schellingb/TinySoundFont/$C/tsf.h && patch vendor/tsf.h < vendor/tsf.patch
```
