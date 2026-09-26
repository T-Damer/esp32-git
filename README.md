# esp32-git

A minimal git client for ESP32 chips (ESP32-C3 / ESP32-S3) that operates on a
git repository stored on SD card and syncs with a remote over Wi-Fi.

Goal: an Obsidian-style vault lives on the device's storage; this library lets
firmware apps commit, pull, and push that vault using the real git object
model — so the same history is readable on a PC with stock git.

## Scope (deliberately tiny)

Implemented:

- `clone` — fetch refs + objects from remote into SD storage
- `fetch` / `pull` — fast-forward only; refuses to merge or rebase
- `push` — fast-forward only; fails if remote has diverged
- `add` — stage file(s) into the index
- `commit` — write blob/tree/commit objects from the staged index
- `status` — staged vs unstaged vs untracked classification
- `init` — create `.git/` skeleton on storage

Not implemented (by design): branches beyond HEAD, merge/rebase, diff or
patch generation, submodules, hooks, packfile *writing* (see below), tags,
stash.

## Design

- **Loose objects only.** Every object is zlib-deflated and SHA-1 addressed,
  written under `.git/objects/xx/yyyy...` exactly like stock git. A PC can
  `git fsck` and `git log` the result.
- **Index:** a simple flat list of staged paths + blob SHAs (`.git/index`
  format v2 subset). Staged/unstaged/untracked classification walks the
  worktree and compares against HEAD's tree.
- **Trees:** one tree per commit covering the tracked root (flat-ish; nested
  directories become nested trees).
- **Commits:** standard commit object (tree, parent, author, committer,
  message). No GPG, no encoding tricks — UTF-8 only.
- **Transport:** HTTPS smart-HTTP (`/info/refs?service=git-upload-pack`,
  `git-upload-pack` POST for fetch; `git-receive-pack` for push). TLS via the
  TLS stack already shipped in CrossPoint firmware (wolfSSL). Auth = user +
  token (basic). Advertisements and push responses use the buffer callback;
  fetch can use `request_stream` to write the upload-pack response directly to
  storage.
- **Packfiles:** parse them for fetch (servers may answer with a pack);
  file-backed fetch parsing reads compressed input through a 1 KiB window and
  keeps the OFS_DELTA offset index in a temporary sidecar file. The temporary
  `.git/esp32git-pack.tmp` and `.idx` files are removed after parsing. Pushes
  still send loose objects only, which git servers accept for small payloads
  via `git-receive-pack`.
- **RAM budget:** the HTTP response is no longer buffered as one allocation;
  the pack reader still limits an individual inflated object and delta result
  to 64 KiB, with the current object/base/delta working set resident.

For a private GitHub repository, use its HTTPS `.git` URL with an
`esp32git_remote` whose `user` is `x-access-token` and `token` is a
repository-scoped token with Contents access. Pass it to `esp32git_clone_url`,
`esp32git_fetch_url_auth`, and `esp32git_push_url_auth`; keep the token out of
the URL and repository files.

For a large vault, use `esp32git_sync_url`. The first call makes a shallow
partial clone (one commit, trees, no blobs) and downloads the files a filter
selects (Markdown/TXT by default); later calls sync both ways:

1. files created, edited or deleted on the device since the last sync are
   found by the library itself — a snapshot in `.git/esp32git-present` tells a
   deleted note from one that was never downloaded, and `stat` (size + mtime)
   avoids rehashing unchanged files;
2. those changes are set aside (`.git/esp32git-stash/`, resumed after a crash),
   the branch fast-forwards and new files download;
3. each change is reapplied: a file the remote left alone takes the device
   version; a file changed on both sides keeps the remote version and gets the
   device version as `<name> (conflict <tag> <time>).<ext>`; an edit wins over a
   deletion;
4. the result is committed and pushed, retrying from step 1 when the remote
   moves in between. Nothing local is lost on any failure.

Downloads batch 48 files per upload-pack request (`ofs-delta`, so the
streaming pack reader always has a delta's base) and retry transient network
errors; a batch that holds a blob too large for the pack reader is split until
only that blob is fetched on its own. On a 1,850-note vault the first download
takes about 40 requests instead of 1,850. `esp32git_download_missing_matching_url`
exposes the same download with any filter and a progress callback, and
`esp32git_download_path_url` fetches one book or attachment by path.

The storage port needs streaming I/O and `rename`; `list_dir` (to notice new
files) and `stat` (to skip rehashing) are optional. The server must allow
`filter`, `shallow` and wants of reachable object ids, as GitHub does.

## Building blocks (already present in CrossPoint firmware)

| need      | reuse                                        |
| --------- | -------------------------------------------- |
| deflate/inflate | miniz / uzlib (vendored)               |
| SHA-1     | wolfSSL (already linked)                     |
| HTTPS     | wolfSSL / SecureNet HAL                      |
| JSON/config | ArduinoJson                                |

## Layout

```
include/
  esp32_git.h        public API (Repo, add/commit/push/pull/clone/status)
src/
  sha1.cpp           streaming SHA-1 (or thin wolfSSL wrapper)
  zlib.cpp           raw-deflate / inflate helpers over miniz
  object.cpp         loose object read/write/parse (blob/tree/commit/index)
  index.cpp          staged index read/write
  refs.cpp           HEAD + refs/heads read/write, fast-forward checks
  transport_http.cpp smart-HTTP fetch/push
src/
  CMakeLists.txt     host unit tests + PlatformIO library registration
test/
  test_objects.cpp   golden-object vectors generated by real git
```

## Status

Bootstrap. API header and object model are being implemented against golden
vectors produced by desktop git (`test/` fixtures).
