#!/usr/bin/env python3
"""Checks which phone photos and videos have original-quality copies in Google Photos.

Subcommands:
  login    Sign in with an OAuth desktop client from your Google Cloud project.
  pick     Select items in the Google Photos picker and record their metadata.
  takeout  Hash the media in Google Takeout archives or extracted folders.
  report   Compare device_manifest.jsonl with the picked items and Takeout
           hashes, and write a report and a delete list.

Only the Python standard library is required. See README.md for the procedure.
"""

import argparse
import base64
import collections
import csv
import glob
import hashlib
import http.server
import json
import os
import secrets
import sys
import tarfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
import zipfile

import media

SCOPE = "https://www.googleapis.com/auth/photospicker.mediaitems.readonly"
AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
PICKER_API = "https://photospicker.googleapis.com/v1"
DEFAULT_TOKEN = os.path.join(
  os.path.expanduser("~"), ".config", "photos_backup", "token.json"
)

# Storage saver resizes videos above 1080p and photos above 16 MP. A cloud copy
# that keeps a resolution above these limits, with margin, was not stored with
# Storage saver or Express quality.
STORAGE_SAVER_VIDEO_EDGES = (1152, 2048)
STORAGE_SAVER_PHOTO_PIXELS = 1 << 24
PICKER_TYPES = {"photo": "PHOTO", "video": "VIDEO"}
STATUSES = (
  "original", "original_resolution", "backed_up_unverified", "downscaled",
  "mismatch", "ambiguous", "not_found",
)


class ApiError(Exception):
  pass


def request_json(request):
  try:
    with urllib.request.urlopen(request, timeout=120) as response:
      body = response.read()
  except urllib.error.HTTPError as error:
    detail = error.read().decode("utf-8", "replace")
    raise ApiError(f"{error.code} {error.reason}: {detail}") from None
  return json.loads(body) if body else {}


def post_form(url, fields):
  data = urllib.parse.urlencode(fields).encode()
  return request_json(urllib.request.Request(url, data=data, method="POST"))


SETUP_HELP = """No OAuth client file found. Create one once in Google Cloud:

  1. Create a project: https://console.cloud.google.com/projectcreate
  2. Enable the Photos Picker API in it:
     https://console.cloud.google.com/apis/library/photospicker.googleapis.com
  3. Configure sign-in at https://console.cloud.google.com/auth/overview
     (audience External); under Audience, add your Google account as a test
     user.
  4. At https://console.cloud.google.com/auth/clients, create a client of type
     Desktop app and download its JSON from the dialog that appears.
  5. Put the client_secret_....json file next to verify.py, or pass
     --client-secret PATH.
"""


def client_secret_path(path):
  """Returns path, or the one client_secret*.json file next to verify.py."""
  if path:
    if not os.path.exists(path):
      sys.exit(f"{path} does not exist.\n\n{SETUP_HELP}")
    return path
  directory = os.path.dirname(os.path.abspath(__file__))
  matches = sorted(glob.glob(os.path.join(directory, "client_secret*.json")))
  if len(matches) > 1:
    sys.exit("Several client_secret*.json files next to verify.py; choose one "
             "with --client-secret")
  if not matches:
    sys.exit(SETUP_HELP)
  return matches[0]


def load_client(path):
  with open(path, encoding="utf-8") as file:
    data = json.load(file)
  client = data.get("installed")
  if not client:
    sys.exit(f"{path} is not a Desktop app OAuth client; see README.md")
  return client["client_id"], client["client_secret"]


def save_token(path, token):
  os.makedirs(os.path.dirname(path), exist_ok=True)
  descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
  with os.fdopen(descriptor, "w", encoding="utf-8") as file:
    json.dump(token, file, indent=2)


def query_fields(url):
  query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
  return {key: values[0] for key, values in query.items()}


class RedirectListener:
  """Receives Google's redirect after consent on a free loopback port."""

  def __init__(self):
    self.fields = {}
    self.received = threading.Event()
    listener = self

    class Handler(http.server.BaseHTTPRequestHandler):
      def do_GET(self):
        fields = query_fields(self.path)
        if "code" in fields or "error" in fields:
          listener.fields = fields
          listener.received.set()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write(b"Sign-in finished. You can close this tab.")

      def log_message(self, *args):
        pass

    self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    self.redirect_uri = f"http://127.0.0.1:{self.server.server_port}/"
    threading.Thread(target=self.server.serve_forever, daemon=True).start()

  def wait(self, timeout):
    self.received.wait(timeout)
    self.server.shutdown()
    self.server.server_close()
    return self.fields


def login(args):
  client_id, client_secret = load_client(client_secret_path(args.client_secret))
  # With --paste nothing listens: the browser shows a connection error, and
  # its address bar holds the code.
  listener = None if args.paste else RedirectListener()
  redirect_uri = listener.redirect_uri if listener else "http://127.0.0.1:8765/"
  verifier = secrets.token_urlsafe(64)
  challenge = base64.urlsafe_b64encode(
    hashlib.sha256(verifier.encode()).digest()
  ).rstrip(b"=").decode()
  state = secrets.token_urlsafe(16)
  parameters = {
    "client_id": client_id,
    "redirect_uri": redirect_uri,
    "response_type": "code",
    "scope": SCOPE,
    "code_challenge": challenge,
    "code_challenge_method": "S256",
    "state": state,
    "access_type": "offline",
    "prompt": "select_account consent",
  }
  if args.account:
    parameters["login_hint"] = args.account
  url = AUTH_URL + "?" + urllib.parse.urlencode(parameters)
  print("Open this address and sign in with the account that holds the backups:")
  print(f"\n{url}\n")
  if listener:
    if not args.no_browser:
      webbrowser.open(url)
    fields = listener.wait(600)
  else:
    fields = query_fields(
      input("Paste the full address of the page Google redirected to: ").strip()
    )
  if fields.get("state") != state or "code" not in fields:
    sys.exit(f"Sign-in failed: {fields.get('error', 'no authorization code')}")
  reply = post_form(TOKEN_URL, {
    "code": fields["code"],
    "client_id": client_id,
    "client_secret": client_secret,
    "redirect_uri": redirect_uri,
    "grant_type": "authorization_code",
    "code_verifier": verifier,
  })
  if "refresh_token" not in reply:
    sys.exit("Google did not return a refresh token; run login again")
  save_token(args.token, {
    "client_id": client_id,
    "client_secret": client_secret,
    "refresh_token": reply["refresh_token"],
    "access_token": reply["access_token"],
    "expires_at": time.time() + reply["expires_in"],
  })
  print(f"Signed in. Token saved to {args.token}")


class PickerApi:
  def __init__(self, token_path):
    if not os.path.exists(token_path):
      sys.exit(f"No token at {token_path}; run `verify.py login` first")
    self.token_path = token_path
    with open(token_path, encoding="utf-8") as file:
      self.token = json.load(file)

  def access_token(self, refresh=False):
    if refresh or self.token["expires_at"] - 60 < time.time():
      reply = post_form(TOKEN_URL, {
        "client_id": self.token["client_id"],
        "client_secret": self.token["client_secret"],
        "refresh_token": self.token["refresh_token"],
        "grant_type": "refresh_token",
      })
      self.token["access_token"] = reply["access_token"]
      self.token["expires_at"] = time.time() + reply["expires_in"]
      save_token(self.token_path, self.token)
    return self.token["access_token"]

  def call(self, method, path, params=None, body=None):
    url = PICKER_API + path
    if params:
      url += "?" + urllib.parse.urlencode(params)
    data = None if body is None else json.dumps(body).encode()
    for attempt in range(6):
      request = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": "Bearer " + self.access_token(refresh=attempt == 1),
        "Content-Type": "application/json",
      })
      try:
        return request_json(request)
      except ApiError as error:
        code = str(error)[:3]
        if attempt == 5 or code not in ("401", "429", "500", "502", "503"):
          raise
        time.sleep(0 if code == "401" else 2 ** attempt)


def seconds(duration, default):
  return float(duration.rstrip("s")) if duration else default


def picked_record(item, session_id):
  media_file = item.get("mediaFile", {})
  metadata = media_file.get("mediaFileMetadata", {})
  record = {
    "id": item["id"],
    "type": item.get("type", "TYPE_UNSPECIFIED"),
    "filename": media_file.get("filename", ""),
    "mimeType": media_file.get("mimeType", ""),
    "createTime": item.get("createTime", ""),
    "width": metadata.get("width"),
    "height": metadata.get("height"),
    "session": session_id,
  }
  video = metadata.get("videoMetadata")
  if video:
    record["processingStatus"] = video.get("processingStatus", "UNSPECIFIED")
  return record


def pick(args):
  api = PickerApi(args.token)
  session = api.call(
    "POST", "/sessions", body={"pickingConfig": {"maxItemCount": "2000"}}
  )
  session_id = session["id"]
  print("Open this link, signed in as the account that holds the backups, and")
  print("select the items to check (up to 2,000 per session):")
  print(f"\n{session['pickerUri']}\n")
  if not args.no_browser:
    webbrowser.open(session["pickerUri"])
  items = []
  try:
    polling = session.get("pollingConfig", {})
    deadline = time.time() + seconds(polling.get("timeoutIn"), 3600)
    while not session.get("mediaItemsSet"):
      if time.time() > deadline:
        sys.exit("The picker session expired before a selection was made")
      time.sleep(seconds(polling.get("pollInterval"), 5))
      session = api.call("GET", f"/sessions/{session_id}")
      polling = session.get("pollingConfig", polling)
    page_token = None
    while True:
      params = {"sessionId": session_id, "pageSize": 100}
      if page_token:
        params["pageToken"] = page_token
      page = api.call("GET", "/mediaItems", params)
      items += [picked_record(item, session_id)
                for item in page.get("mediaItems", [])]
      page_token = page.get("nextPageToken")
      if not page_token:
        break
  finally:
    try:
      api.call("DELETE", f"/sessions/{session_id}")
    except ApiError:
      pass
  picked = {record["id"]: record for record in media.read_jsonl(args.picked)}
  picked.update((record["id"], record) for record in items)
  media.write_jsonl(args.picked, sorted(
    picked.values(), key=lambda record: (record["filename"], record["id"])
  ))
  print(f"Recorded {len(items)} items; {args.picked} now lists {len(picked)}")


def takeout_members(source):
  """Yields (member name, opener) for each media file in a Takeout source."""
  if os.path.isdir(source):
    for directory, _, files in os.walk(source):
      for name in sorted(files):
        if media.media_kind(name):
          path = os.path.join(directory, name)
          yield os.path.relpath(path, source), lambda path=path: open(path, "rb")
  elif zipfile.is_zipfile(source):
    with zipfile.ZipFile(source) as archive:
      for info in archive.infolist():
        if not info.is_dir() and media.media_kind(info.filename):
          yield info.filename, lambda info=info: archive.open(info)
  elif tarfile.is_tarfile(source):
    with tarfile.open(source, "r|*") as archive:
      for member in archive:
        if member.isfile() and media.media_kind(member.name):
          yield member.name, lambda member=member: archive.extractfile(member)
  else:
    sys.exit(f"{source} is not a folder, zip, or tar archive")


def takeout(args):
  done = {(record["source"], record["member"])
          for record in media.read_jsonl(args.output)}
  hashed = 0
  with open(args.output, "a", encoding="utf-8") as output:
    for source in args.sources:
      label = os.path.basename(os.path.normpath(source))
      for member, opener in takeout_members(source):
        if (label, member) in done:
          continue
        with opener() as file:
          digest, size = media.stream_sha256(file)
        record = {"sha256": digest, "size": size, "source": label,
                  "member": member}
        output.write(json.dumps(record, ensure_ascii=False) + "\n")
        output.flush()
        hashed += 1
        if hashed % 100 == 0:
          print(f"hashed {hashed} files", file=sys.stderr)
  print(f"Hashed {hashed} new files into {args.output}")


def edges(width, height):
  return tuple(sorted((width, height))) if width and height else None


def exceeds_storage_saver(kind, local):
  if kind == "video":
    return (local[0] > STORAGE_SAVER_VIDEO_EDGES[0]
            or local[1] > STORAGE_SAVER_VIDEO_EDGES[1])
  return local[0] * local[1] > STORAGE_SAVER_PHOTO_PIXELS


def classify(manifest, picked, takeout_hashes):
  """Returns (record, status, detail) for every device file.

  original             A byte-identical copy is in the Takeout export.
  original_resolution  The picked cloud copy keeps a resolution Storage saver
                       would have reduced, and videos have finished processing.
  backed_up_unverified A picked cloud copy exists, but its quality is unknown.
  downscaled           The picked cloud copy is smaller than the device file.
  mismatch             The picked cloud copy has other dimensions.
  ambiguous            The file name matches several different files.
  not_found            Nothing picked or exported matches the file.
  """
  exported = {(record["sha256"], record["size"]) for record in takeout_hashes}
  cloud_by_name = collections.defaultdict(list)
  for item in picked:
    cloud_by_name[item["filename"].lower()].append(item)
  local_hashes_by_name = collections.defaultdict(set)
  for record in manifest:
    name = os.path.basename(record["path"]).lower()
    local_hashes_by_name[name].add(record["sha256"])

  results = []
  for record in manifest:
    if (record["sha256"], record["size"]) in exported:
      results.append((record, "original", "byte-identical file in Takeout"))
      continue
    kind = record["kind"]
    name = os.path.basename(record["path"]).lower()
    items = [item for item in cloud_by_name.get(name, ())
             if item["type"] in (PICKER_TYPES[kind], "TYPE_UNSPECIFIED")]
    if not items:
      results.append((record, "not_found", "no picked item or Takeout file"))
      continue
    local_count = len(local_hashes_by_name[name])
    if len(items) > 1 or local_count > 1:
      results.append((record, "ambiguous",
                      f"{len(items)} picked items and {local_count} different "
                      "device files share this name"))
      continue
    item = items[0]
    local = edges(record.get("width"), record.get("height"))
    cloud = edges(item.get("width"), item.get("height"))
    if not local or not cloud:
      results.append((record, "backed_up_unverified", "dimensions unavailable"))
    elif local == cloud:
      size = f"{local[1]}x{local[0]}"
      if not exceeds_storage_saver(kind, local):
        results.append((record, "backed_up_unverified",
                        f"{size} is within Storage saver limits"))
      elif kind == "video" and item.get("processingStatus") != "READY":
        results.append((record, "backed_up_unverified",
                        f"video processing is {item.get('processingStatus')}"))
      else:
        results.append((record, "original_resolution",
                        f"cloud copy keeps {size}"))
    elif cloud[0] <= local[0] and cloud[1] <= local[1]:
      results.append((record, "downscaled",
                      f"cloud {cloud[1]}x{cloud[0]}, device {local[1]}x{local[0]}"))
    else:
      results.append((record, "mismatch",
                      f"cloud {cloud[1]}x{cloud[0]}, device {local[1]}x{local[0]}"))
  return results


def report(args):
  manifest = media.read_jsonl(args.manifest)
  if not manifest:
    sys.exit(f"No device manifest at {args.manifest}")
  picked = media.read_jsonl(args.picked)
  takeout_hashes = media.read_jsonl(args.takeout)
  if not picked and not takeout_hashes:
    sys.exit("Run `pick` or `takeout` first; there is nothing to compare with")
  results = classify(manifest, picked, takeout_hashes)
  order = {status: index for index, status in enumerate(STATUSES)}
  results.sort(key=lambda result: (order[result[1]], result[0]["path"]))

  with open(args.report, "w", newline="", encoding="utf-8") as file:
    writer = csv.writer(file)
    writer.writerow(["status", "kind", "bytes", "path", "detail"])
    for record, status, detail in results:
      writer.writerow([status, record["kind"], record["size"], record["path"],
                       detail])

  allowed = {"original"} | ({"original_resolution"} if args.trust_resolution
                            else set())
  kinds = {"photo", "video"} if args.kind == "all" else {args.kind}
  deletions = [
    {"path": record["path"], "sha256": record["sha256"],
     "size": record["size"], "status": status}
    for record, status, _ in results
    if status in allowed and record["kind"] in kinds
  ]
  media.write_jsonl(args.delete_list, deletions)

  totals = collections.defaultdict(lambda: [0, 0])
  for record, status, _ in results:
    totals[(status, record["kind"])][0] += 1
    totals[(status, record["kind"])][1] += record["size"]
  print(f"{'status':22} {'kind':6} {'files':>7} {'GB':>8}")
  for status in STATUSES:
    for kind in ("video", "photo"):
      if (status, kind) in totals:
        count, size = totals[(status, kind)]
        print(f"{status:22} {kind:6} {count:7} {size / 1e9:8.2f}")
  freed = sum(entry["size"] for entry in deletions)
  print(f"\nReport: {args.report}")
  print(f"Delete list: {args.delete_list} "
        f"({len(deletions)} files, {freed / 1e9:.2f} GB; "
        f"statuses: {', '.join(sorted(allowed))}; kinds: {', '.join(sorted(kinds))})")


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest="command", required=True)

  command = commands.add_parser("login", help="sign in to Google")
  command.add_argument("--client-secret",
                       help="Desktop app OAuth client JSON (default: the "
                            "client_secret*.json file next to verify.py)")
  command.add_argument("--account", help="suggested Google account")
  command.add_argument("--paste", action="store_true",
                       help="paste the redirect address instead of listening "
                            "for it, for machines without a local browser")
  command.add_argument("--no-browser", action="store_true")
  command.add_argument("--token", default=DEFAULT_TOKEN)
  command.set_defaults(run=login)

  command = commands.add_parser("pick", help="select items in Google Photos")
  command.add_argument("--picked", default="picked.jsonl")
  command.add_argument("--no-browser", action="store_true")
  command.add_argument("--token", default=DEFAULT_TOKEN)
  command.set_defaults(run=pick)

  command = commands.add_parser("takeout", help="hash a Google Takeout export")
  command.add_argument("sources", nargs="+",
                       help="Takeout .zip or .tgz files or extracted folders")
  command.add_argument("--output", default="takeout.jsonl")
  command.set_defaults(run=takeout)

  command = commands.add_parser("report", help="cross-reference and report")
  command.add_argument("--manifest", default=media.MANIFEST)
  command.add_argument("--picked", default="picked.jsonl")
  command.add_argument("--takeout", default="takeout.jsonl")
  command.add_argument("--report", default="report.csv")
  command.add_argument("--delete-list", default="delete_list.jsonl")
  command.add_argument("--kind", choices=("video", "photo", "all"),
                       default="video", help="kinds to put on the delete list")
  command.add_argument("--trust-resolution", action="store_true",
                       help="also delete original_resolution files")
  command.set_defaults(run=report)

  args = parser.parse_args()
  args.run(args)


if __name__ == "__main__":
  main()
