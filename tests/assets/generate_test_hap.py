#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
"""Generate a minimal HAPA test clip for DanceHAP CI and local testing.

Produces ``sample_hapa_5s.mov``: a 5-second HAP Alpha clip with an
animated checker pattern exercising the full alpha range, plus a sine
wave audio track.

Requirements:
  - Python 3.9+ with numpy
  - ffmpeg (with HAP encoder support) in PATH

Usage:
  python generate_test_hap.py [--output PATH] [--width 256] [--height 256]
                              [--fps 30] [--duration 5.0]

The script is idempotent: re-running overwrites the output file
deterministically (same ffmpeg version → same bytes).

Verification (run automatically at the end):
  ffprobe -show_entries stream=codec_name,codec_tag_string tests/assets/sample_hapa_5s.mov
  Expected: codec_name=hap, codec_tag_string=Hap5 (= HAPA / DXT5-alpha)
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# Force UTF-8 stdout/stderr — Windows runners default to cp1252 and crash on
# any non-ASCII print (e.g. arrow, accented letters). Idempotent on Py3.7+.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    import numpy as np
except ImportError:
    sys.exit("ERROR: numpy is required. Install with: pip install numpy")

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

DEFAULT_OUTPUT = Path(__file__).resolve().parent / "sample_hapa_5s.mov"
DEFAULT_WIDTH = 256
DEFAULT_HEIGHT = 256
DEFAULT_FPS = 30
DEFAULT_DURATION = 5.0
AUDIO_FREQ = 440.0  # Hz (A4)
AUDIO_SAMPLE_RATE = 44100


# ---------------------------------------------------------------------------
# Frame generation
# ---------------------------------------------------------------------------

def make_frame(width: int, height: int, frame_idx: int, n_frames: int) -> np.ndarray:
    """Generate one RGBA frame: animated 8x8 checker with varying alpha.

    Pattern:
      - Even cells: opaque warm gradient (R=220, G/B oscillate).
      - Odd cells:  blue with alpha sweeping 60–200 across time + position.

    This exercises the full alpha range so HAPA decoding/alpha tests
    can detect regressions in the alpha channel.
    """
    rgba = np.zeros((height, width, 4), dtype=np.uint8)
    cell_w = width // 8
    cell_h = height // 8

    # Normalised time [0, 2π)
    phase = (frame_idx / n_frames) * 2.0 * np.pi

    for row in range(8):
        for col in range(8):
            y0, y1 = row * cell_h, (row + 1) * cell_h
            x0, x1 = col * cell_w, (col + 1) * cell_w
            checker = (row + col) % 2

            if checker == 0:
                # Opaque warm square
                r = 220
                g = int(40 + 80 * (0.5 + 0.5 * np.sin(phase)))
                b = int(60 + 100 * (0.5 + 0.5 * np.cos(phase)))
                a = 255
            else:
                # Blue square with animated alpha
                r = 30
                g = 80
                b = 200
                # Alpha sweeps 60..200, phase-shifted per cell
                local_phase = phase + (row + col) * 0.3
                a = int(130 + 70 * np.sin(local_phase))

            rgba[y0:y1, x0:x1] = [r, g, b, a]

    return rgba


# ---------------------------------------------------------------------------
# ffmpeg encoding
# ---------------------------------------------------------------------------

def find_ffmpeg() -> str:
    """Locate the ffmpeg binary."""
    path = shutil.which("ffmpeg")
    if path is None:
        sys.exit("ERROR: ffmpeg not found in PATH. Install ffmpeg first.")
    return path


def encode_hapa(
    frames_iter,
    width: int,
    height: int,
    fps: int,
    duration: float,
    output: Path,
) -> None:
    """Encode raw RGBA frames to HAPA .mov with sine-wave audio.

    Buffers all frames in memory (small at default 256x256x5s = ~39 MB) and
    uses subprocess.run(input=...) so ffmpeg's stderr is always captured,
    even if ffmpeg dies early (e.g. HAP encoder missing, bad args). The
    previous streaming-pipe approach masked ffmpeg's error message behind a
    BrokenPipeError on the write side.
    """
    ffmpeg = find_ffmpeg()
    n_frames = int(fps * duration)

    cmd = [
        ffmpeg,
        "-y",                       # overwrite
        "-hide_banner",
        "-loglevel", "error",
        # --- Video input (raw RGBA via stdin) ---
        "-f", "rawvideo",
        "-pix_fmt", "rgba",
        "-s", f"{width}x{height}",
        "-r", str(fps),
        "-i", "pipe:0",
        # --- Audio input (generated sine wave) ---
        "-f", "lavfi",
        "-i", f"sine=frequency={AUDIO_FREQ}:duration={duration}"
              f":sample_rate={AUDIO_SAMPLE_RATE}",
        # --- Video encoder: HAP Alpha ---
        "-c:v", "hap",
        "-format", "hap_alpha",
        "-pix_fmt", "rgba",
        # --- Audio encoder: AAC ---
        "-c:a", "aac",
        "-b:a", "64k",
        # --- Container ---
        "-shortest",
        "-movflags", "+faststart",
        str(output),
    ]

    # Buffer frames then send in one shot — lets ffmpeg's stderr surface on
    # failure instead of breaking the pipe on our side.
    frames_bytes = b"".join(frame.tobytes() for frame in frames_iter)

    result = subprocess.run(
        cmd,
        input=frames_bytes,
        capture_output=True,
        timeout=60,
    )

    if result.returncode != 0:
        stderr = result.stderr.decode(errors="replace")
        sys.exit(
            f"ERROR: ffmpeg failed (exit {result.returncode}):\n{stderr}\n"
            f"Hint: ensure ffmpeg was built with the HAP encoder "
            f"(check 'ffmpeg -encoders | grep hap')."
        )


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def verify_output(output: Path, expected_duration: float) -> bool:
    """Run ffprobe and check the output is valid HAPA with audio."""
    ffprobe = shutil.which("ffprobe")
    if ffprobe is None:
        print("WARNING: ffprobe not found — skipping verification.")
        return True

    result = subprocess.run(
        [
            ffprobe, "-v", "error",
            "-show_entries",
            "stream=codec_name,codec_tag_string,width,height,duration,"
            "channels,sample_rate",
            "-show_entries", "format=duration",
            "-of", "json",
            str(output),
        ],
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        print(f"ffprobe failed:\n{result.stderr}")
        return False

    import json
    data = json.loads(result.stdout)
    streams = data.get("streams", [])
    video_streams = [s for s in streams if s.get("codec_name") == "hap"]
    audio_streams = [s for s in streams if s.get("codec_name") in ("aac", "mp3", "pcm_s16le")]

    if not video_streams:
        print("FAIL: no HAP video stream found.")
        return False

    v = video_streams[0]
    tag = v.get("codec_tag_string", "").strip()
    # Hap5 = HAPA (DXT5 alpha), Hap1 = HAP, HapY = HAPQ
    if tag not in ("Hap5", "HapA"):
        print(f"FAIL: expected HAPA (Hap5), got codec_tag '{tag}'.")
        return False

    if not audio_streams:
        print("FAIL: no audio stream found.")
        return False

    dur = float(data.get("format", {}).get("duration", 0))
    if abs(dur - expected_duration) > 0.5:
        print(f"FAIL: duration {dur:.2f}s ≠ expected {expected_duration:.1f}s.")
        return False

    print(f"OK: {output.name} — HAPA {v['width']}x{v['height']}, "
          f"{dur:.1f}s, audio={audio_streams[0]['codec_name']}, "
          f"{output.stat().st_size} bytes")
    return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a minimal HAPA test clip for DanceHAP."
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"Output path (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--duration", type=float, default=DEFAULT_DURATION)
    args = parser.parse_args()

    n_frames = int(args.fps * args.duration)
    print(f"Generating {n_frames} frames ({args.width}x{args.height} RGBA) "
          f"at {args.fps} fps → {args.output}")

    def frame_iterator():
        for i in range(n_frames):
            yield make_frame(args.width, args.height, i, n_frames)

    encode_hapa(
        frame_iterator(),
        args.width,
        args.height,
        args.fps,
        args.duration,
        args.output,
    )

    if not verify_output(args.output, args.duration):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
