# Usenet Backend Test Status - 2026-07-11

Status: current field-test report for `feature/usenet-nyuu-batch-upload`.

## Environment

- Host project path: `/home/edwin/myProjects/nashawk`
- Daemon test config: `/tmp/nashawk-webui-real/config`
- Download path: `/tmp/nashawk-webui-real/downloads`
- Web UI path used for tests: `/home/edwin/myProjects/nashawk/web/public_html`
- Usenet credentials are loaded from local `.env`; credentials are intentionally
  not recorded here.
- Working Nyuu runtime: Node.js `v24.18.0` from nvm, with Nyuu installed under
  `/home/edwin/.nvm/versions/node/v24.18.0`.

The daemon must be started with a PATH that resolves `nyuu` and `node` from a
compatible Node.js installation. The known-good test command used this form:

```sh
set -a
. ./.env
set +a

source ~/.nvm/nvm.sh
nvm use 24.18.0

TRANSMISSION_WEB_HOME=/home/edwin/myProjects/nashawk/web/public_html \
PATH=/home/edwin/.nvm/versions/node/v24.18.0/bin:/usr/local/bin:/usr/bin:/bin \
./build/daemon/transmission-daemon -f \
  -g /tmp/nashawk-webui-real/config \
  -w /tmp/nashawk-webui-real/downloads \
  --incomplete-dir /tmp/nashawk-webui-real/incomplete \
  -r 127.0.0.1 \
  -p 19091 \
  -T \
  --usenet-enabled \
  --usenet-upload-concurrency 40 \
  --usenet-eviction-enabled \
  --usenet-eviction-min-age-minutes 5 \
  --usenet-cache-size-mib 0 \
  --log-level=info \
  -e /tmp/nashawk-webui-real/daemon.log
```

## Important Environment Finding

The earlier global `/usr/local/bin/nyuu` installation used a native
`yencode.node` module compiled for Node ABI `115` while one daemon environment
resolved Node.js `v24.18.0`, ABI `137`. This produced runtime upload errors
like:

```text
ERR_DLOPEN_FAILED
was compiled against a different Node.js version using NODE_MODULE_VERSION 115.
This version of Node.js requires NODE_MODULE_VERSION 137.
```

That mismatch caused large numbers of Nyuu upload failures. Reinstalling Nyuu
inside the Node 24 nvm environment fixed the ABI mismatch. `nyuu --version`
alone is not a sufficient validation because it does not necessarily load the
native yEnc module.

The daemon now performs a startup check that resolves the active `nyuu` binary
from `PATH` and uses the active `node` binary to load that Nyuu installation's
bundled `node_modules/yencode` module. A synthetic bad Nyuu installation was
tested and startup failed with an actionable error before any upload work was
started.

## Code State Under Test

The branch currently includes:

- batch Nyuu upload support;
- deterministic staged filenames, `<piece-hash>.piece`;
- token-expanded Nyuu message IDs, `{fnamebase}@nashawk.local`;
- a conservative per-Nyuu-process upload connection cap;
- shared Usenet IO slot accounting for upload, download, and Nyuu check
  connections;
- startup validation that the active Nyuu installation can load its bundled
  yEnc module with the active Node.js runtime;
- batch-failure fallback to conservative single-file Nyuu upload;
- readback verification before marking a failed upload unavailable;
- local piece eviction for Usenet-available pieces;
- Web/RPC observability for session and torrent Usenet state.

## Automated Validation

The following checks passed after the current changes:

```sh
cmake --build build -j2
./build/tests/libtransmission/libtransmission-test \
  --gtest_filter='*Usenet*:RpcTest.sessionStatsIncludesUsenetRuntimeSnapshot:RpcTest.torrentGetIncludesUsenetPieceSummary:SettingsTest.usenetEvictionSettingsHaveConservativeDefaults:SettingsTest.canLoadAndSaveUsenetEvictionSettings'
```

Additional startup checks passed:

- a daemon with the known-good Node 24/Nyuu environment started successfully and
  served RPC/Web until the test timeout stopped it;
- a daemon with a synthetic Nyuu installation whose bundled `yencode` module
  throws at load time failed startup immediately with the expected actionable
  message.

The focused test run executed 17 tests and all passed.

## Manual Nyuu Validation

Direct Nyuu tests were run against the configured Usenet provider.

Successful checks:

- one Nyuu process uploaded multiple small files with token-expanded Message-IDs;
- one Nyuu process uploaded 40 staged files;
- two Nyuu batch processes uploaded concurrently in a shell test;
- 40 files of 128 KiB each produced 40 NZB article segments when Nyuu was given
  a padded `article-size`, confirming one file/piece to one article in that
  configuration.

An important correction was made during this testing: setting Nyuu
`article-size` equal to the raw piece size is not the correct safety margin.
Nyuu receives a larger article-size value so yEnc expansion and article overhead
do not split a piece into multiple articles.

## Real Torrent Upload Test

The latest successful real test used:

```text
[ANi] BLACK TORCH 闇黑燈火 - 02 [1080P][Baha][WEB-DL][AAC AVC][CHT].mp4
```

Observed state after upload:

```text
piece_count: 857
available: 857
failed: 0
uploading: 0
unknown: 0
```

After the Node/Nyuu ABI mismatch was fixed, no new Nyuu `signal 11`,
`ERR_DLOPEN_FAILED`, readback failure, checksum failure, or upload failure was
observed for this torrent in the checked log window.

## Eviction Test

Automatic local piece eviction was enabled with:

```text
usenet_eviction_enabled: true
usenet_eviction_min_age_minutes: 5
usenet_cache_size_mib: 0
```

The daemon logged:

```text
Evicted 850 Usenet-backed local piece(s), 445377660 byte(s)
```

After eviction, the torrent's local `Have` dropped substantially while the
Usenet manifest still reported all pieces available. This matches the intended
VPS/storage-constrained behavior.

## Usenet Readback / Serving Test

After eviction, remote peer requests triggered Usenet download activity.

Observed via RPC:

```text
usenet_download_in_flight: 3
usenet_download_queue_size: 2
usenet_io_active: 1
```

After waiting, the queue drained:

```text
usenet_download_in_flight: 0
usenet_download_queue_size: 0
usenet_io_active: 0
```

During this period:

- torrent `Downloaded` did not increase;
- local `Have` increased from roughly 52 MiB to 66.84 MiB;
- no Usenet download, `BODY`, unexpected-size, or checksum errors were logged.

This is strong evidence that missing local pieces were restored from Usenet and
then became locally serviceable again.

## Clean Config End-to-End Test

A clean local end-to-end test was run with generated test data rather than a
third-party copyrighted torrent.

Test layout:

```text
Nashawk config: /tmp/nashawk-clean-e2e/config
Nashawk downloads: /tmp/nashawk-clean-e2e/downloads
Leecher config: /tmp/nashawk-clean-e2e-leecher/config
Leecher downloads: /tmp/nashawk-clean-e2e-leecher/downloads
Test data: 5.24 MB, 40 pieces, 128 KiB each
Info hash: 88264867c7f7d991eaa52580a80e3a70b45dd008
```

The source file was first served through a local HTTP webseed. Python's default
`http.server` was not sufficient because it returned `200` to Range requests;
Transmission requires `206` for webseed piece fetches. A tiny local Range server
was used instead.

Observed sequence:

1. Nashawk downloaded the 40-piece generated torrent from the local Range
   webseed.
2. The Usenet manifest reached:

   ```text
   piece_count: 40
   available: 40
   ```

3. No Nyuu, upload, readback, checksum, `BODY`, or unexpected-size errors were
   logged during the checked window.
4. Automatic eviction ran on the next 5-minute eviction interval and logged:

   ```text
   Evicted 40 Usenet-backed local piece(s), 5242880 byte(s)
   ```

5. Nashawk's local `Have` dropped to none while the manifest stayed 40/40
   `available`.
6. A second local daemon was started with a torrent carrying the same infohash
   but no webseed URL.
7. Local peer discovery connected the leecher to Nashawk. The leecher saw
   Nashawk as `Done 100%` even though Nashawk had no local pieces before the
   request path began.
8. The leecher downloaded from Nashawk to 100%.
9. Nashawk restored pieces from Usenet during serving and ended with:

   ```text
   Have: 5.24 MB verified
   Uploaded: 5.25 MB
   Ratio: 1.00
   ```

This validates the intended clean path:

```text
piece completion -> Usenet upload -> local eviction -> Usenet-backed peer
advertisement -> peer request -> Usenet readback -> BT upload to peer
```

## Known Dirty Test Data

Several earlier manifests under `/tmp/nashawk-webui-real/config/usenet-pieces`
contain failed or stuck states from pre-fix runs. They should not be used as
merge-quality evidence.

Examples include manifests with many `failed` or `uploading` entries created
before the Node 24 Nyuu install was fixed. Fresh validation should use a new
torrent or a clean config directory.

## Known Issues / Caveats

- Nyuu is still an external process. Environment drift between `node`, `npm`,
  and Nyuu native modules can break startup even when Nashawk itself is built
  correctly. This is intentional because the mode cannot safely run without a
  working Nyuu/yEnc runtime.
- Current success logs for Usenet piece restore are trace-level. With
  `--log-level=info`, successful restores are best observed through RPC counters
  and local `Have` changes.
- Eviction currently runs on a 5-minute interval. This is acceptable for
  continuous storage control, but it makes manual tests slower and should be
  kept in mind when debugging.
- On restart, the first eviction scan may run before all resumed torrents are
  loaded. The next periodic scan evicts correctly.

## Current Assessment

The core loop has now been demonstrated in a real daemon run:

1. BitTorrent downloads pieces.
2. Nashawk uploads pieces to Usenet.
3. The manifest reaches all pieces `available`.
4. Local eviction removes Usenet-backed pieces.
5. Peer requests trigger Usenet readback.
6. Readback restores local pieces without increasing BT downloaded bytes.

The intended Usenet-backed storage behavior has now passed both real-daemon
field validation and a clean-config generated-data end-to-end test. The feature
branch is a reasonable merge candidate, with the remaining caveats above
documented for follow-up hardening rather than blocking the first merge.
