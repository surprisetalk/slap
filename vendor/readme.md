# vendor

Single-header decoders from [mackron/dr_libs](https://github.com/mackron/dr_libs), public domain (Unlicense) or MIT-0, at commit `35b5c392cc15456736c440a68dbe073178bbffe0`:

- `dr_mp3.h` v0.7.4: `read-mp3`
- `dr_flac.h` v0.13.4: `read-flac`
- `dr_wav.h` v0.14.6: `read-wav`

From [schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont) at commit `853a0a171759f1ddba0de1442133a75912bbeffa`, for the coming `render-midi`; nothing includes them yet:

- `tsf.h` v0.9 (MIT): SoundFont 2 synthesizer
- `tml.h` (zlib): MIDI file loader

slap.c defines `DR_MP3_IMPLEMENTATION`, `DR_FLAC_IMPLEMENTATION` and `DR_WAV_IMPLEMENTATION` before it includes them. To update, replace each file whole:

```bash
C=<commit>; for f in dr_mp3 dr_flac dr_wav; do curl -sfL -o vendor/$f.h https://raw.githubusercontent.com/mackron/dr_libs/$C/$f.h; done
C=<commit>; for f in tsf tml; do curl -sfL -o vendor/$f.h https://raw.githubusercontent.com/schellingb/TinySoundFont/$C/$f.h; done
```
