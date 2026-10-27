# vendor

Single-header decoders from [mackron/dr_libs](https://github.com/mackron/dr_libs), public domain (Unlicense) or MIT-0, at commit `35b5c392cc15456736c440a68dbe073178bbffe0`:

- `dr_mp3.h` v0.7.4: `read-mp3`
- `dr_flac.h` v0.13.4: `read-flac`

slap.c defines `DR_MP3_IMPLEMENTATION` and `DR_FLAC_IMPLEMENTATION` before it includes them. To update, replace each file whole:

```bash
C=<commit>; for f in dr_mp3 dr_flac; do curl -sfL -o vendor/$f.h https://raw.githubusercontent.com/mackron/dr_libs/$C/$f.h; done
```
