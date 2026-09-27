#!/usr/bin/env python3
"""Deletes phone files on a verify.py delete list after re-checking each checksum.

Run it on the phone. It is a dry run unless --execute is given. A file is only
removed when its size and SHA-256 still match the delete list, and every
removal is appended to a journal. Removing the local file does not delete the
Google Photos copy; only deleting inside Google Photos does that.
"""

import argparse
import collections
import datetime
import json
import os
import sys

import media


def check(root, entry, execute):
  relative = entry["path"]
  if os.path.isabs(relative) or ".." in relative.split("/"):
    return "unsafe path"
  path = os.path.join(root, relative)
  try:
    size = os.stat(path).st_size
  except FileNotFoundError:
    return "missing"
  if size != entry["size"]:
    return "size changed"
  if media.file_sha256(path) != entry["sha256"]:
    return "checksum changed"
  if not execute:
    return "would delete"
  os.remove(path)
  return "deleted"


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument("delete_list", help="delete_list.jsonl from verify.py")
  parser.add_argument("--root", default=media.DEVICE_ROOT)
  parser.add_argument("--execute", action="store_true",
                      help="delete files instead of listing them")
  parser.add_argument("--journal", default="delete_journal.jsonl")
  args = parser.parse_args()

  totals = collections.defaultdict(lambda: [0, 0])
  journal = open(args.journal, "a", encoding="utf-8") if args.execute else None
  try:
    for entry in media.read_jsonl(args.delete_list):
      result = check(args.root, entry, args.execute)
      totals[result][0] += 1
      totals[result][1] += entry["size"]
      if result not in ("would delete", "deleted"):
        print(f"skipped {entry['path']}: {result}", file=sys.stderr)
      if journal:
        journal.write(json.dumps({
          "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "result": result, **entry,
        }, ensure_ascii=False) + "\n")
        journal.flush()
  finally:
    if journal:
      journal.close()
  for result, (count, size) in sorted(totals.items()):
    print(f"{result:16} {count:6} files {size / 1e9:8.2f} GB")
  if not args.execute:
    print("Dry run; nothing was deleted. Add --execute to delete.")


if __name__ == "__main__":
  main()
