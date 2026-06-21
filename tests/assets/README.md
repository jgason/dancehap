# Test Assets — DanceHAP

Generated test media for CI and local development.

## `sample_hapa_5s.mov`

A minimal **HAP Alpha (HAPA)** clip used by Phase 1.2+ demux/decode unit
tests and CI smoke tests.

| Property       | Value                          |
|----------------|--------------------------------|
| Format         | HAP Alpha (FourCC `Hap5`)      |
| Resolution     | 256 × 256                      |
| Frame rate     | 30 fps                         |
| Duration       | 5.0 s                          |
| Video pattern  | Animated 8×8 checker, varying alpha (60–255) |
| Audio          | AAC, 440 Hz sine wave, mono, 64 kbps |
| File size      | ~650 KB (< 1 MB)               |
| Container      | QuickTime `.mov`               |

### Generation

```bash
python tests/assets/generate_test_hap.py
```

Requirements: Python 3.9+, numpy, ffmpeg (with HAP encoder).

The script is **idempotent and deterministic**: running it again
overwrites `sample_hapa_5s.mov` with byte-identical output (assuming
the same ffmpeg version).

### Verification

```bash
ffprobe -show_entries stream=codec_name,codec_tag_string,width,height,duration \
  tests/assets/sample_hapa_5s.mov
# Expect: codec_name=hap, codec_tag_string=Hap5 (= HAPA)
```

The generator script also runs this check automatically after encoding.

---

## Why this clip is NOT committed to git

The `.mov` file is excluded by `.gitignore` (`tests/assets/*.mov`).
Instead, CI regenerates it before each test run via
`generate_test_hap.py`. Rationale:

1. **Reproducibility**: the script produces the same output on every run.
   A committed binary could silently drift from the generator.
2. **Repository hygiene**: no opaque binary diffs in commits.
3. **Size**: ~650 KB per version — trivial to regenerate, unnecessary to
   store in git history.

---

## Design decision: how the HAPA clip is produced

Three approaches were evaluated:

### Option 1 — Pure Python HAP frame writer (no ffmpeg)

Write raw HAP/DXT5 frames + Snappy compression + QuickTime container
entirely in Python.

| Pros                                   | Cons                                                |
|----------------------------------------|-----------------------------------------------------|
| Zero external deps (no ffmpeg)         | Must reimplement Snappy, DXT5, QuickTime muxer      |
| Full control over every byte           | ~2000+ lines of codec/container code                |
|                                        | High maintenance burden, bug-prone                  |
|                                        | Still needs ffprobe for verification                |

**Verdict**: rejected. The complexity is unjustifiable for a test asset.
Reimplementing a video codec from scratch would be a larger effort than
the plugin itself, and bugs in the hand-rolled encoder could mask real
HAP decoder bugs.

### Option 2 — ffmpeg HAP encoder (CHOSEN) ✅

Use numpy to generate animated RGBA checker frames, pipe raw to ffmpeg's
`hap` encoder (`-format hap_alpha`), add a sine-wave audio track.

| Pros                                   | Cons                                          |
|----------------------------------------|-----------------------------------------------|
| ffmpeg is preinstalled on all GitHub Actions runners | Requires ffmpeg locally for generation |
| HAP encoder is well-tested, spec-compliant          | numpy dependency for frame generation   |
| ~120 lines of Python, trivial to maintain           |                                         |
| Same tool used by the HAP ecosystem (Jokyo, Vidvox) |                                         |
| Produces verified HAPA + audio in < 1s              |                                         |

**Verdict**: chosen. This is the standard approach used by the HAP
ecosystem (ffmpeg's HAP encoder is the reference open-source encoder).
ffmpeg is already available on CI runners, and the numpy + ffmpeg
combination is lightweight. The exception to the "use Jokyo for HAP
encoding" rule is justified: this is for CI test reproducibility, not
production encoding.

### Option 3 — Git LFS with a pre-encoded clip

Commit a pre-encoded `.mov` via Git LFS.

| Pros                              | Cons                                                  |
|-----------------------------------|-------------------------------------------------------|
| No runtime generation needed      | Git LFS quota on free GitHub accounts (1 GB storage)  |
|                                   | Binary diffs are opaque                               |
|                                   | Can silently drift from any generator                 |
|                                   | Requires LFS setup for every contributor              |

**Verdict**: rejected. The quota concern is real for a free GitHub
account, and the regeneration cost is negligible (< 1 second). LFS adds
friction without meaningful benefit here.

---

## Adding new test assets

Place generators in this directory. Follow the same pattern:

1. Script is deterministic and idempotent.
2. Add `*.mov` / `*.mp4` patterns to `.gitignore` (already covered).
3. CI regenerates before tests.
4. Document properties and verification method here.
