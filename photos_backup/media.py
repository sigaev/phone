"""Definitions shared by the photos_backup scripts."""

import hashlib
import json
import os

# Shared storage as the phone's apps see it.
DEVICE_ROOT = "/storage/emulated/0"
MANIFEST = os.path.join(
  os.path.dirname(os.path.abspath(__file__)), "device_manifest.jsonl"
)

PHOTO_EXTENSIONS = frozenset(
  {".avif", ".bmp", ".dng", ".gif", ".heic", ".heif", ".jpeg", ".jpg", ".png",
   ".webp"}
)
VIDEO_EXTENSIONS = frozenset(
  {".3g2", ".3gp", ".avi", ".flv", ".m2ts", ".m4v", ".mkv", ".mov", ".mp4",
   ".mts", ".ts", ".webm", ".wmv"}
)


def media_kind(name):
  extension = os.path.splitext(name)[1].lower()
  if extension in PHOTO_EXTENSIONS:
    return "photo"
  if extension in VIDEO_EXTENSIONS:
    return "video"
  return None


def stream_sha256(file):
  """Returns the SHA-256 hex digest and byte count of an open binary file."""
  digest = hashlib.sha256()
  size = 0
  while chunk := file.read(1 << 22):
    digest.update(chunk)
    size += len(chunk)
  return digest.hexdigest(), size


def file_sha256(path):
  with open(path, "rb") as file:
    return stream_sha256(file)[0]


def read_jsonl(path):
  if not os.path.exists(path):
    return []
  with open(path, encoding="utf-8") as file:
    return [json.loads(line) for line in file if line.strip()]


def write_jsonl(path, records):
  """Replaces path atomically with one compact JSON object per line."""
  temporary = path + ".tmp"
  with open(temporary, "w", encoding="utf-8") as file:
    for record in records:
      file.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
      file.write("\n")
  os.replace(temporary, path)
