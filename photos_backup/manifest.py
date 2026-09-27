#!/usr/bin/env python3
"""Records the size, SHA-256, and dimensions of every photo and video on the phone.

Run it on the phone, from Termux or its Debian PRoot, to refresh
device_manifest.jsonl. Files whose size and modification time are unchanged
keep their previous checksum, so later runs only hash new or changed files.
Video dimensions come from ffprobe when it is installed.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import struct
import subprocess
import sys

import media

# Regenerable previews, and app-private directories Android does not expose.
SKIPPED_DIRECTORIES = frozenset({".thumbnails", "Android/data", "Android/obb"})
JPEG_FRAME_MARKERS = frozenset(range(0xC0, 0xD0)) - {0xC4, 0xC8, 0xCC}
FFPROBE = shutil.which("ffprobe")


def walk(root):
  for directory, subdirectories, files in os.walk(root):
    relative = os.path.relpath(directory, root)
    prefix = "" if relative == "." else relative + "/"
    subdirectories[:] = sorted(
      name for name in subdirectories
      if name not in SKIPPED_DIRECTORIES
      and prefix + name not in SKIPPED_DIRECTORIES
    )
    for name in sorted(files):
      kind = media.media_kind(name)
      if kind:
        yield prefix + name, kind


def jpeg_size(file):
  file.seek(2)
  while True:
    byte = file.read(1)
    if not byte:
      return None
    if byte != b"\xff":
      continue
    marker = file.read(1)
    while marker == b"\xff":
      marker = file.read(1)
    if not marker:
      return None
    code = marker[0]
    if code == 0x01 or 0xD0 <= code <= 0xD8:
      continue
    if code in (0xD9, 0xDA):
      return None
    length = file.read(2)
    if len(length) < 2:
      return None
    if code in JPEG_FRAME_MARKERS:
      frame = file.read(5)
      if len(frame) < 5:
        return None
      height, width = struct.unpack(">HH", frame[1:5])
      return width, height
    file.seek(struct.unpack(">H", length)[0] - 2, os.SEEK_CUR)


def webp_size(header):
  chunk = header[12:16]
  if chunk == b"VP8 " and len(header) >= 30:
    width, height = struct.unpack("<HH", header[26:30])
    return width & 0x3FFF, height & 0x3FFF
  if chunk == b"VP8L" and len(header) >= 25:
    bits = int.from_bytes(header[21:25], "little")
    return (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
  if chunk == b"VP8X" and len(header) >= 30:
    return (
      int.from_bytes(header[24:27], "little") + 1,
      int.from_bytes(header[27:30], "little") + 1,
    )
  return None


def image_size(path):
  """Returns the stored width and height of a JPEG, PNG, GIF, or WebP image."""
  with open(path, "rb") as file:
    header = file.read(30)
    if header.startswith(b"\x89PNG\r\n\x1a\n") and len(header) >= 24:
      return struct.unpack(">II", header[16:24])
    if header[:6] in (b"GIF87a", b"GIF89a"):
      return struct.unpack("<HH", header[6:10])
    if header[:4] == b"RIFF" and header[8:12] == b"WEBP":
      return webp_size(header)
    if header[:2] == b"\xff\xd8":
      return jpeg_size(file)
  return None


def video_info(path):
  if not FFPROBE:
    return {}
  result = subprocess.run(
    [FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
     "stream=width,height:format=duration", "-of", "json", path],
    capture_output=True, text=True,
  )
  if result.returncode:
    return {}
  data = json.loads(result.stdout)
  info = {}
  stream = (data.get("streams") or [{}])[0]
  if stream.get("width") and stream.get("height"):
    info["width"], info["height"] = stream["width"], stream["height"]
  duration = data.get("format", {}).get("duration")
  if duration:
    info["duration"] = round(float(duration), 3)
  return info


def describe(root, relative, kind, previous):
  path = os.path.join(root, relative)
  status = os.stat(path)
  old = previous.get(relative)
  if old and old["size"] == status.st_size and old["mtime"] == int(status.st_mtime):
    return old
  record = {
    "path": relative,
    "kind": kind,
    "size": status.st_size,
    "mtime": int(status.st_mtime),
    "sha256": media.file_sha256(path),
  }
  if kind == "video":
    record.update(video_info(path))
  else:
    size = image_size(path)
    if size:
      record["width"], record["height"] = size
  return record


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument("--root", default=media.DEVICE_ROOT)
  parser.add_argument("--output", default=media.MANIFEST)
  parser.add_argument("--jobs", type=int, default=4)
  args = parser.parse_args()

  if not FFPROBE:
    print("ffprobe not found; video dimensions will be omitted", file=sys.stderr)
  previous = {record["path"]: record for record in media.read_jsonl(args.output)}
  files = list(walk(args.root))
  records = []
  hashed_bytes = 0

  def run(entry):
    try:
      return describe(args.root, *entry, previous)
    except OSError as error:
      print(f"skipped {entry[0]}: {error}", file=sys.stderr)
      return None

  with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    for count, record in enumerate(pool.map(run, files), 1):
      if record:
        records.append(record)
        hashed_bytes += record["size"]
      if count % 250 == 0 or count == len(files):
        print(f"{count}/{len(files)} files, {hashed_bytes / 1e9:.1f} GB",
              file=sys.stderr)
  media.write_jsonl(args.output, records)
  print(f"wrote {len(records)} records to {args.output}", file=sys.stderr)


if __name__ == "__main__":
  main()
