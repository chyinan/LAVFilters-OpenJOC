#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenJOC contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate deterministic, private-capture PCM/FLAC fixtures without external tools.

The JOC fixture is supplied separately. Optional --ffmpeg verifies the generated
FLAC using an independent decoder; ffmpeg is not needed to create the fixtures.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FRAMES = 8192
RATE = 48000


def pcm_value(frame, channel, bits):
    # Channel-distinct, bipolar, deliberately well below full scale.
    coarse = ((frame * (29 + 2 * channel) + channel * 997) % 8191) - 4095
    extra_bits = bits - 16
    # Populate the low precision bits so an accidental 16-bit round-trip cannot
    # pass the packed24 / 24-valid-in32 exact-payload checks.
    fine = (frame * 13 + channel * 19 + 7) & ((1 << extra_bits) - 1)
    return (coarse << extra_bits) + fine


def integers(channels, bits, container=None):
    container = container or bits
    return b"".join((pcm_value(frame, channel, bits) << (container - bits)).to_bytes(
        container // 8, "little", signed=True)
        for frame in range(FRAMES) for channel in range(channels))


def floats(bits):
    return b"".join(struct.pack("<f" if bits == 32 else "<d", pcm_value(frame, channel, 16) / 32768)
                    for frame in range(FRAMES) for channel in range(2))


def wave_file(payload, channels, bits, floating=False, valid=None, mask=None, rate=RATE):
    extensible = valid is not None or mask is not None
    tag = 3 if floating else 1
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 0xfffe if extensible else tag, channels, rate,
                      rate * block, block, bits)
    if extensible:
        fmt += struct.pack("<HHI", 22, valid or bits, mask or 0)
        fmt += struct.pack("<IHH8s", tag, 0, 0x10, bytes.fromhex("800000aa00389b71"))
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def crc(data, width, polynomial):
    value = 0
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (width - 1)) else 0)) & ((1 << width) - 1)
    return value


def flac_verbatim(pcm):
    """FLAC fixed 1024-frame blocks, 48 kHz, independent stereo, signed 16-bit."""
    frames = []
    for index, start in enumerate(range(0, FRAMES, 1024)):
        assert index < 128
        header = bytes([0xff, 0xf8, 0xaa, 0x18, index])
        frame = header + bytes([crc(header, 8, 0x07)])
        for channel in range(2):
            frame += b"\x02"  # VERBATIM subframe, no wasted bits
            frame += b"".join(pcm[(sample * 2 + channel) * 2:(sample * 2 + channel + 1) * 2][::-1]
                              for sample in range(start, start + 1024))
        frame += crc(frame, 16, 0x8005).to_bytes(2, "big")
        frames.append(frame)
    sizes = [len(frame) for frame in frames]
    info = struct.pack(">HH", 1024, 1024)
    info += min(sizes).to_bytes(3, "big") + max(sizes).to_bytes(3, "big")
    info += ((RATE << 44) | (1 << 41) | (15 << 36) | FRAMES).to_bytes(8, "big")
    info += hashlib.md5(pcm).digest()
    assert len(info) == 34
    return b"fLaC\x80\x00\x00\x22" + info + b"".join(frames)


def prepare(output, joc_fixture=None, ffmpeg=None):
    output.mkdir(parents=True, exist_ok=True)
    s16 = integers(2, 16)
    s24 = integers(6, 24)
    cases = {
        "pcm.s16": (wave_file(s16, 2, 16), s16),
        "pcm.f32": (wave_file(floats(32), 2, 32, floating=True), floats(32)),
        "pcm.f64": (wave_file(floats(64), 2, 64, floating=True), floats(32)),
        "pcm.s24": (wave_file(s24, 6, 24, valid=24, mask=0x3f), s24),
        "pcm.s24in32": (wave_file(integers(6, 24, 32), 6, 32, valid=24, mask=0x3f), s24),
        "pcm.s24.96k": (wave_file(s24, 6, 24, valid=24, mask=0x3f, rate=96000), s24),
        "pcm.f32.96k": (wave_file(floats(32), 2, 32, floating=True, rate=96000), floats(32)),
    }
    for name, (wave, expected) in cases.items():
        (output / (name + ".wav")).write_bytes(wave)
        (output / (name + ".expected.pcm")).write_bytes(expected)
    (output / "pcm.s24in32.input.pcm").write_bytes(integers(6, 24, 32))
    (output / "pcm.control.flac").write_bytes(flac_verbatim(s16))
    (output / "pcm.control.expected.pcm").write_bytes(s16)
    if joc_fixture:
        target = output / "joc.lifecycle.ec3"
        if joc_fixture.resolve() != target.resolve():
            shutil.copyfile(joc_fixture, target)
    if ffmpeg:
        for filename, codec in [("pcm.control.flac", "pcm_s16le"), ("pcm.s24.wav", "pcm_s24le"),
                                ("pcm.f64.wav", "pcm_f32le"), ("pcm.s24.96k.wav", "pcm_s24le"),
                                ("pcm.f32.96k.wav", "pcm_f32le")]:
            decoded = subprocess.run([ffmpeg, "-v", "error", "-i", str(output / filename),
                                      "-f", {"pcm_s16le": "s16le", "pcm_s24le": "s24le", "pcm_f32le": "f32le"}[codec],
                                      "-acodec", codec, "-"], check=True, stdout=subprocess.PIPE).stdout
            oracle = output / (filename.rsplit(".", 1)[0] + ".expected.pcm")
            if decoded != oracle.read_bytes():
                raise RuntimeError("Independent decode mismatch: " + filename)
    paths = sorted(path for path in output.iterdir() if path.suffix in {".wav", ".flac", ".pcm", ".ec3"})
    manifest = {path.name: {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "bytes": path.stat().st_size}
                for path in paths}
    (output / "pcm-track-fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--joc-fixture", type=Path, required=True)
    parser.add_argument("--ffmpeg", help="Optional independent verification executable")
    args = parser.parse_args()
    if not args.joc_fixture.is_file():
        parser.error("--joc-fixture must name an existing E-AC-3/JOC elementary stream")
    manifest = prepare(args.output_dir, args.joc_fixture, args.ffmpeg)
    print("PCM_TRACK_FIXTURES_READY files=" + str(len(manifest)))


if __name__ == "__main__":
    main()
