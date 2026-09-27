# Google Photos backup verification

These scripts find the phone's photos and videos that have an original-quality
copy in Google Photos, so that only those local files are deleted.

- `manifest.py` runs on the phone. It writes `device_manifest.jsonl` with the
  path, size, SHA-256, and dimensions of every photo and video in shared
  storage, including WhatsApp media under `Android/media`.
- `verify.py` runs on any computer with Python 3.10 or later and only uses the
  standard library. It signs in to Google Photos, collects what is stored
  there, and compares it with the manifest.
- `delete_verified.py` runs on the phone. It deletes the files on the delete
  list after re-checking each file's size and SHA-256.

## What can be verified

Since March 2025, Google's Library API only returns items the calling app
uploaded, so no program can list a library directly. Google Photos contents
can be checked in two ways:

- **Google Takeout** exports the stored files. A device file whose SHA-256 and
  size match an exported file has a byte-identical copy in Google Photos, which
  can only come from an Original quality upload. Anything that Google
  re-encoded does not match and is never marked `original`.
- **Photos Picker API**: you select items in a Google Photos picker, and the
  API returns each item's file name, type, and dimensions. Downloads from this
  API do not return original files: videos are a
  [transcoded version](https://developers.google.com/photos/picker/guides/media-items),
  and photos lose their location metadata. Checksums therefore cannot be
  compared, but dimensions can. Storage saver resizes videos above 1080p and
  photos above 16 MP. When the cloud copy of a file above those limits keeps
  the full resolution, it was uploaded in original quality. For smaller files,
  the Picker can only show that a copy exists.

`report` combines both sources. Each device file receives one status:

| Status | Meaning | Deleted by default |
|---|---|---|
| `original` | Byte-identical file in the Takeout export | Yes |
| `original_resolution` | Picked cloud copy keeps a resolution above Storage saver limits; videos have finished processing | Only with `--trust-resolution` |
| `backed_up_unverified` | Picked cloud copy exists, but its quality cannot be shown | No |
| `downscaled` | Picked cloud copy is smaller than the device file (Storage saver) | No |
| `mismatch` | Picked cloud copy has other dimensions | No |
| `ambiguous` | Several picked items or different device files share the name | No |
| `not_found` | Nothing picked or exported matches | No |

Picked items are matched to device files by file name. Takeout files are
matched by checksum, so their names do not matter.

## One-time Google Cloud setup

1. In the [Google Cloud console](https://console.cloud.google.com/), create a
   project.
2. Under **APIs & Services → Library**, enable the **Google Photos Picker API**.
3. Under **Google Auth Platform**, fill in **Branding**. In **Audience**, keep
   the app in **Testing** as an **External** app and add the account that
   holds the backups as a test user.
4. Under **Clients**, create an OAuth client with application type
   **Desktop app**, and download its JSON from the dialog that appears after
   creation. Put the downloaded `client_secret_….json` file next to
   `verify.py`; `login` finds it there, or accepts `--client-secret PATH`.
   The file is ignored by Git.

Google expires refresh tokens for apps in Testing after seven days. Run
`login` again when requests start failing with an authorization error.

## Checking the backups

On the computer, from this directory:

```sh
python3 verify.py login --account sigaev@gmail.com
```

A browser opens for consent. On a machine without a local browser, add
`--paste`, open the printed address anywhere, and paste the address of the page
Google redirects to. That page fails to load, and its address holds the code.
The token is saved to `~/.config/photos_backup/token.json`.

To use Takeout, export Google Photos from
[takeout.google.com](https://takeout.google.com/) with the same account. You
may limit the export to the **Photos from 2024** through **Photos from 2026**
albums that cover the phone's media. Then hash the downloaded archives without
extracting them:

```sh
python3 verify.py takeout ~/Downloads/takeout-*.tgz
```

`takeout` also accepts `.zip` archives and extracted folders, and skips files
that are already in `takeout.jsonl` when it is run again.

To use the Picker:

```sh
python3 verify.py pick
```

Open the printed link in a browser signed in to the same account, select the
items, and confirm the selection. A session holds up to 2,000 items. Run `pick` again
for more; every run adds to `picked.jsonl`.

Then compare:

```sh
python3 verify.py report
python3 verify.py report --trust-resolution
```

`report` writes every device file's status to `report.csv` and the files that
are safe to delete to `delete_list.jsonl`. The delete list includes videos
only; use `--kind all` or `--kind photo` for photos.

## Deleting on the phone

Copy `delete_list.jsonl` to the phone, then run a dry run from this directory:

```sh
python3 delete_verified.py delete_list.jsonl
```

Check the summary, then delete:

```sh
python3 delete_verified.py delete_list.jsonl --execute
```

Files whose size or checksum changed since the manifest are skipped. Every
result is appended to `delete_journal.jsonl`. Deleting a local file with this
script does not delete its Google Photos copy. Before a large deletion, delete
one file and confirm that it still opens in Google Photos on the web.

## Refreshing the manifest

On the phone, run:

```sh
python3 manifest.py
```

Unchanged files keep their checksums, so only new or modified files are hashed.
Video dimensions require `ffprobe` (`apt-get install ffmpeg`). HEIC dimensions
are not read; those files can only be verified through Takeout.

Run the tests with `python3 -m unittest` in this directory.
