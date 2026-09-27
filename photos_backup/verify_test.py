"""Tests for the photos_backup scripts. Run with `python3 -m unittest` here."""

import argparse
import contextlib
import hashlib
import http.server
import io
import json
import os
import tarfile
import tempfile
import threading
import unittest
import urllib.parse
import urllib.request
import zipfile
from unittest import mock

import delete_verified
import media
import verify


def record(path, kind, content, width=None, height=None):
  entry = {"path": path, "kind": kind, "size": len(content),
           "sha256": hashlib.sha256(content).hexdigest()}
  if width:
    entry.update(width=width, height=height)
  return entry


def picked(filename, kind, width, height, processing="READY"):
  item = {"id": filename + kind, "type": verify.PICKER_TYPES[kind],
          "filename": filename, "width": width, "height": height}
  if kind == "video":
    item["processingStatus"] = processing
  return item


class ClassifyTest(unittest.TestCase):
  def test_statuses(self):
    manifest = [
      record("DCIM/Camera/exported.mp4", "video", b"a", 3840, 2160),
      record("DCIM/Camera/uhd.mp4", "video", b"b", 3840, 2160),
      record("DCIM/Camera/rotated.mp4", "video", b"c", 2160, 3840),
      record("DCIM/Camera/hd.mp4", "video", b"d", 1920, 1080),
      record("DCIM/Camera/saver.mp4", "video", b"e", 3840, 2160),
      record("DCIM/Camera/processing.mp4", "video", b"f", 3840, 2160),
      record("DCIM/Camera/big.jpg", "photo", b"g", 8160, 6144),
      record("DCIM/Camera/small.jpg", "photo", b"h", 4080, 3072),
      record("A/same.jpg", "photo", b"i", 8160, 6144),
      record("B/same.jpg", "photo", b"j", 8160, 6144),
      record("DCIM/Camera/missing.mp4", "video", b"k", 3840, 2160),
      record("DCIM/Camera/odd.mp4", "video", b"l", 3840, 2160),
    ]
    cloud = [
      picked("uhd.mp4", "video", 3840, 2160),
      picked("rotated.mp4", "video", 3840, 2160),
      picked("hd.mp4", "video", 1920, 1080),
      picked("saver.mp4", "video", 1920, 1080),
      picked("processing.mp4", "video", 3840, 2160, "PROCESSING"),
      picked("BIG.JPG", "photo", 8160, 6144),
      picked("small.jpg", "photo", 4080, 3072),
      picked("same.jpg", "photo", 8160, 6144),
      picked("odd.mp4", "video", 4096, 2160),
    ]
    exported = [{"sha256": manifest[0]["sha256"], "size": 1}]
    statuses = {entry["path"].split("/")[-1] + ":" + entry["path"][0]: status
                for entry, status, _ in verify.classify(manifest, cloud,
                                                        exported)}
    self.assertEqual(statuses, {
      "exported.mp4:D": "original",
      "uhd.mp4:D": "original_resolution",
      "rotated.mp4:D": "original_resolution",
      "hd.mp4:D": "backed_up_unverified",
      "saver.mp4:D": "downscaled",
      "processing.mp4:D": "backed_up_unverified",
      "big.jpg:D": "original_resolution",
      "small.jpg:D": "backed_up_unverified",
      "same.jpg:A": "ambiguous",
      "same.jpg:B": "ambiguous",
      "missing.mp4:D": "not_found",
      "odd.mp4:D": "mismatch",
    })

  def test_takeout_needs_matching_size(self):
    entry = record("x.mp4", "video", b"abc", 3840, 2160)
    exported = [{"sha256": entry["sha256"], "size": 4}]
    self.assertEqual(verify.classify([entry], [], exported)[0][1], "not_found")


class FakeGoogle(http.server.BaseHTTPRequestHandler):
  polls = 0

  def reply(self, body):
    data = json.dumps(body).encode()
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

  def authorized(self):
    if self.headers.get("Authorization") != "Bearer access":
      self.send_error(401)
      return False
    return True

  def do_POST(self):
    length = int(self.headers.get("Content-Length", 0))
    body = self.rfile.read(length).decode()
    if self.path == "/token":
      fields = urllib.parse.parse_qs(body)
      assert fields["client_secret"] == ["secret"], fields
      if fields["grant_type"] == ["authorization_code"]:
        assert fields["code"] == ["the-code"], fields
        assert fields["code_verifier"][0], fields
      self.reply({"access_token": "access", "refresh_token": "refresh",
                  "expires_in": 3600})
    elif self.path == "/v1/sessions" and self.authorized():
      assert json.loads(body)["pickingConfig"]["maxItemCount"] == "2000"
      self.reply({"id": "s1", "pickerUri": "https://picker/s1",
                  "pollingConfig": {"pollInterval": "0.01s",
                                    "timeoutIn": "60s"},
                  "mediaItemsSet": False})

  def do_GET(self):
    if not self.authorized():
      return
    url = urllib.parse.urlparse(self.path)
    query = urllib.parse.parse_qs(url.query)
    if url.path == "/v1/sessions/s1":
      FakeGoogle.polls += 1
      self.reply({"id": "s1", "mediaItemsSet": FakeGoogle.polls > 1})
    elif url.path == "/v1/mediaItems":
      assert query["sessionId"] == ["s1"]
      if "pageToken" not in query:
        self.reply({"nextPageToken": "p2", "mediaItems": [{
          "id": "1", "type": "VIDEO", "createTime": "2025-08-16T17:36:57Z",
          "mediaFile": {"filename": "PXL_1.mp4", "mimeType": "video/mp4",
                        "mediaFileMetadata": {
                          "width": 3840, "height": 2160,
                          "videoMetadata": {"processingStatus": "READY"}}}}]})
      else:
        self.reply({"mediaItems": [{
          "id": "2", "type": "PHOTO",
          "mediaFile": {"filename": "PXL_2.jpg", "mimeType": "image/jpeg",
                        "mediaFileMetadata": {"width": 4080,
                                              "height": 3072}}}]})

  def do_DELETE(self):
    if self.authorized():
      self.reply({})

  def log_message(self, *args):
    pass


class GoogleFlowTest(unittest.TestCase):
  def setUp(self):
    self.server = http.server.HTTPServer(("127.0.0.1", 0), FakeGoogle)
    threading.Thread(target=self.server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{self.server.server_port}"
    self.directory = tempfile.TemporaryDirectory()
    self.token = os.path.join(self.directory.name, "config", "token.json")
    self.client = os.path.join(self.directory.name, "client_secret.json")
    with open(self.client, "w") as file:
      json.dump({"installed": {"client_id": "id", "client_secret": "secret"}},
                file)
    patches = [
      mock.patch.object(verify, "TOKEN_URL", base + "/token"),
      mock.patch.object(verify, "PICKER_API", base + "/v1"),
      mock.patch("webbrowser.open", self.follow_redirect),
    ]
    for patch in patches:
      patch.start()
      self.addCleanup(patch.stop)

  def tearDown(self):
    self.server.shutdown()
    self.server.server_close()
    self.directory.cleanup()

  def follow_redirect(self, url):
    """Plays the browser: consents and follows Google's loopback redirect."""
    query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
    if "redirect_uri" not in query:
      return
    self.assertEqual(query["scope"], [verify.SCOPE])
    self.assertEqual(query["code_challenge_method"], ["S256"])
    target = query["redirect_uri"][0] + "?" + urllib.parse.urlencode(
      {"code": "the-code", "state": query["state"][0]})
    threading.Thread(target=urllib.request.urlopen, args=(target,)).start()

  def test_login_then_pick(self):
    with contextlib.redirect_stdout(io.StringIO()):
      verify.login(argparse.Namespace(
        client_secret=self.client, account="sigaev@gmail.com", paste=False,
        no_browser=False, token=self.token))
      self.assertEqual(os.stat(self.token).st_mode & 0o777, 0o600)
      picked_path = os.path.join(self.directory.name, "picked.jsonl")
      verify.pick(argparse.Namespace(
        token=self.token, picked=picked_path, no_browser=True))
    items = media.read_jsonl(picked_path)
    self.assertEqual([item["filename"] for item in items],
                     ["PXL_1.mp4", "PXL_2.jpg"])
    self.assertEqual(items[0]["processingStatus"], "READY")
    self.assertEqual((items[1]["width"], items[1]["height"]), (4080, 3072))

  def test_login_with_pasted_redirect(self):
    redirect = "http://127.0.0.1:8765/?code=the-code&state=fixed"
    with (mock.patch.object(verify.secrets, "token_urlsafe",
                            return_value="fixed"),
          mock.patch("builtins.input", return_value=redirect),
          contextlib.redirect_stdout(io.StringIO())):
      verify.login(argparse.Namespace(
        client_secret=self.client, account=None, paste=True,
        no_browser=True, token=self.token))
    with open(self.token) as file:
      self.assertEqual(json.load(file)["refresh_token"], "refresh")


class TakeoutTest(unittest.TestCase):
  def test_folder_zip_and_tar(self):
    with tempfile.TemporaryDirectory() as directory:
      folder = os.path.join(directory, "Takeout", "Google Photos")
      os.makedirs(folder)
      for name, content in (("a.mp4", b"video"), ("a.mp4.json", b"{}")):
        with open(os.path.join(folder, name), "wb") as file:
          file.write(content)
      archive = os.path.join(directory, "takeout-1.zip")
      with zipfile.ZipFile(archive, "w") as output:
        output.writestr("Takeout/Google Photos/b.jpg", b"photo")
      tarball = os.path.join(directory, "takeout-2.tgz")
      with tarfile.open(tarball, "w:gz") as output:
        info = tarfile.TarInfo("Takeout/Google Photos/c.MOV")
        info.size = 5
        output.addfile(info, io.BytesIO(b"movie"))
      hashes = os.path.join(directory, "takeout.jsonl")
      arguments = argparse.Namespace(
        sources=[os.path.join(directory, "Takeout"), archive, tarball],
        output=hashes)
      with contextlib.redirect_stdout(io.StringIO()):
        verify.takeout(arguments)
        verify.takeout(arguments)
      found = {(entry["member"].split("/")[-1], entry["sha256"])
               for entry in media.read_jsonl(hashes)}
      self.assertEqual(found, {
        (name, hashlib.sha256(content).hexdigest())
        for name, content in (("a.mp4", b"video"), ("b.jpg", b"photo"),
                              ("c.MOV", b"movie"))})


class DeleteTest(unittest.TestCase):
  def test_rechecks_before_deleting(self):
    with tempfile.TemporaryDirectory() as root:
      os.makedirs(os.path.join(root, "DCIM"))
      entries = []
      for name, content in (("keep.mp4", b"same"), ("edited.mp4", b"old")):
        with open(os.path.join(root, "DCIM", name), "wb") as file:
          file.write(content)
        entries.append(record("DCIM/" + name, "video", content))
      with open(os.path.join(root, "DCIM", "edited.mp4"), "wb") as file:
        file.write(b"new")
      entries.append(record("../outside.mp4", "video", b"x"))
      self.assertEqual(delete_verified.check(root, entries[0], False),
                       "would delete")
      self.assertTrue(os.path.exists(os.path.join(root, "DCIM", "keep.mp4")))
      self.assertEqual(delete_verified.check(root, entries[0], True),
                       "deleted")
      self.assertFalse(os.path.exists(os.path.join(root, "DCIM", "keep.mp4")))
      self.assertEqual(delete_verified.check(root, entries[1], True),
                       "checksum changed")
      self.assertEqual(delete_verified.check(root, entries[2], True),
                       "unsafe path")
      self.assertEqual(delete_verified.check(root, entries[0], True),
                       "missing")


if __name__ == "__main__":
  unittest.main()
