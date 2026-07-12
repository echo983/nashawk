# Usenet Piece Backend README

Nashawk includes an experimental Usenet-backed piece storage mode for
`transmission-daemon`. The mode lets a node keep normal BitTorrent torrent
metadata while storing verified pieces as Usenet articles. This is intended for
hosts with limited local disk, such as small VPS instances, that still need to
serve torrents after local data has been removed.

Usenet mode is opt-in. When it is not enabled, Nashawk follows the normal
Transmission behavior.

## Status

This backend is functional but experimental. The first implementation has been
validated with local two-daemon BitTorrent tests and real Usenet upload/read
paths.

Current field-test details are recorded in
`archive/Usenet-Backend-Test-Status-2026-07-11.md`, including the Node/Nyuu ABI
issue found during testing, real torrent upload results, local piece eviction,
and Usenet readback validation.

Validated paths:

- startup validation for `nyuu`, Usenet credentials, posting, and readback
- upload of completed local pieces to Usenet
- persisted per-torrent Usenet piece manifests
- peer-facing availability from local pieces plus Usenet-available pieces
- on-demand Usenet fetch when a peer requests a missing local piece
- yEnc decode and piece hash verification before recovered data is served
- recovery after daemon restart for interrupted uploads
- configurable shared Usenet IO limit
- duplicate-content pieces sharing the same deterministic message-id are
  coalesced instead of uploaded repeatedly
- automatic local piece eviction for Usenet-available pieces
- peer-triggered restore of evicted local pieces from Usenet
- read-only Web UI and RPC observability for daemon and torrent Usenet state
- sampled Usenet piece discovery after magnet metadata is available

Current limits:

- Usenet downloads use one worker and share the configured Usenet IO limit with
  uploads.
- Restored pieces are written back into the torrent data path. They can be
  evicted again after the configured minimum age when local eviction is enabled.
- The first request for a cold Usenet-only piece can be rejected while the
  piece is fetched; peers should retry.
- The first local eviction implementation uses hole punching and currently
  skips pieces that span multiple files or filesystems that do not support
  punching sparse holes.
- `usenet_cache_size_mib` is present, but the current implemented eviction pass
  is conservative and age-driven; full size-pressure/LRU policy remains future
  work.
- Discovery runs once for a fresh manifest that does not already contain
  meaningful Usenet piece state. Automatic rediscovery or manual reset controls
  are future work.
- End-to-end Usenet tests are currently manual, not CI automation.
- Windows startup support for this mode is not implemented.

## Storage Model

Nashawk stores a BitTorrent piece in one or more Usenet articles. Pieces at or
below the configured payload limit use one article. Larger pieces are split into
deterministic contiguous parts and remain one logical BitTorrent piece.

Each piece is addressed by a deterministic `Message-ID`. In the current
implementation, the message-id local part is the piece SHA1 hash:

```text
<piece-sha1@nashawk.local>
<piece-sha1.1@nashawk.local>
<piece-sha1.2@nashawk.local>
```

The base article is unchanged for compatibility. Continuations are numbered
from `.1`; a piece may use at most 1024 articles. The final continuation may be
shorter than the configured payload limit.

The Usenet article body is the piece bytes encoded as yEnc. No NZB index is
required for normal operation.

## Local State

The backend keeps Usenet state separate from normal local completion.

Important concepts:

- local piece: the piece exists in the torrent data path and has passed the
  torrent hash check
- Usenet piece: the manifest says the piece has been uploaded and is available
  in Usenet
- servable piece: either the local piece exists or the Usenet manifest says it
  is available

Peer bitfields use servable availability. Torrent progress and local verify
still use local completion. If every piece is servable through local data or
Usenet, the torrent is treated as seed-like for activity, queue direction, and
peer upload-only behavior, but local completion, verification, and tracker
completion semantics remain unchanged.

The Usenet manifest is stored under:

```text
<config-dir>/usenet-pieces/<infohash>.json
```

Manifest states:

- `unknown`: not known to be available in Usenet
- `uploading`: upload was queued or in progress
- `available`: the piece is considered usable from Usenet
- `failed`: recent upload, fetch, decode, or hash verification failed

On startup, interrupted `uploading` states are reset to `unknown`, and existing
local verified pieces are scanned so they can be queued again.

## Configuration

Copy `.env.example` to `.env` in the daemon config directory or repository root
used for local testing:

```text
USENET_HOST=news.example.com
USENET_PORT=563
USENET_TLS=true
USENET_USERNAME=your-username
USENET_PASSWORD=your-password
USENET_FROM=poster@example.com
USENET_GROUP=alt.binaries.example
```

`.env` and `.env.*` are ignored by git. Do not commit real credentials.

`USENET_CONNECTIONS` may exist in `.env`, but daemon Usenet IO concurrency is
controlled by Nashawk's own setting or CLI flag.

## Daemon Options

Enable the backend:

```sh
transmission-daemon --usenet-enabled
```

Validate a provider article size at startup:

```sh
transmission-daemon --usenet-enabled --usenet-check-article-size 2097152
```

`usenet_check_article_size` defaults to `2097152` bytes (2 MiB). This is a
conservative raw piece limit that leaves substantial room for yEnc expansion and
article headers on providers with a roughly 4 MB final article limit. Set it
higher only after the provider startup check confirms that larger payloads can
be posted and read back.

Configure the shared Usenet upload/download IO limit:

```sh
transmission-daemon --usenet-enabled --usenet-upload-concurrency 40
```

The shared Usenet IO limit defaults to 4 and is clamped to 1 through 64. Upload
workers and download workers must acquire a slot from this same limit before
opening Usenet IO, so a setting of 40 will not create more than 40 concurrent
Usenet operations.

For stability with `nyuu` 0.4.x, Nashawk keeps concurrent `nyuu` upload
processes low. Upload workers batch multiple completed pieces into each `nyuu`
process, and Nyuu uses the configured shared Usenet IO limit through its own
multi-connection uploader. Higher `usenet_upload_concurrency` values therefore
raise the upload/download connection budget without requiring one process per
connection. A single Nyuu batch is also locally capped to a conservative number
of upload connections, with Nyuu's post-check connection reserved from the same
shared IO budget.

The equivalent settings keys are:

```json
{
  "usenet_enabled": true,
  "usenet_check_article_size": 2097152,
  "usenet_cache_size_mib": 0,
  "usenet_eviction_enabled": false,
  "usenet_eviction_min_age_minutes": 60,
  "usenet_discovery_enabled": true,
  "usenet_discovery_sample_size": 16,
  "usenet_upload_concurrency": 40
}
```

Local piece eviction is disabled by default. To enable the first conservative
eviction policy:

```sh
transmission-daemon --usenet-enabled \
  --usenet-eviction-enabled \
  --usenet-eviction-min-age-minutes 60 \
  --usenet-cache-size-mib 0
```

`usenet_cache_size_mib` defaults to `0`, which disables size-pressure eviction.
When `usenet_eviction_enabled` is true, age-based eviction may still remove
pieces that are already marked available in the Usenet manifest and have reached
`usenet_eviction_min_age_minutes`.

Usenet discovery is enabled by default when Usenet mode is enabled. It can be
disabled explicitly:

```sh
transmission-daemon --usenet-enabled --no-usenet-discovery
```

The default discovery sample size is 16 pieces:

```sh
transmission-daemon --usenet-enabled --usenet-discovery-sample-size 16
```

## Startup Validation

When `usenet_enabled` is true, daemon startup fails if any required backend
check fails.

Startup checks:

- `nyuu` is present in `PATH`
- the active `node` runtime can load the active Nyuu installation's bundled
  `node_modules/yencode` module
- `.env` or environment variables include required Usenet credentials
- NNTP TCP/TLS connection succeeds
- NNTP authentication succeeds
- posting is permitted
- a small yEnc loopback article can be posted and read back
- the configured article-size check can be posted and read back

Failure messages are intended to be actionable and must not print credentials.

## Upload Flow

When a local piece completes and passes the torrent hash check:

1. Nashawk checks the torrent's Usenet manifest.
2. Pieces already marked `available` or `uploading` are skipped.
3. The full piece is staged into a temporary local file.
4. Upload workers split each staged piece into deterministic `<piece-hash>`,
   `<piece-hash>.1`, and subsequent files in a private batch directory.
5. A temporary `nyuu` config is written with restrictive permissions.
6. `nyuu` posts one yEnc article per staged part using token-expanded
   deterministic message-ids.
7. Only complete logical-piece success makes the manifest entry `available`;
   the manifest also records the article count and uploader payload size.
8. On batch failure, Nashawk retries each piece through the conservative
   single-file Nyuu path.
9. On retry failure, Nashawk attempts per-piece readback before marking failed.
10. Temporary piece files, batch files, and temporary `nyuu` configs are removed.

Upload failure does not change normal torrent completion state.

## Read And Serving Flow

When a peer requests a piece:

1. If the piece exists locally, Nashawk serves it through the normal path.
2. If it does not exist locally but the manifest says it is `available`,
   Nashawk queues an asynchronous Usenet fetch.
3. The current peer request can be rejected while the fetch is in flight.
4. The download worker fetches and decodes the base article, then numbered
   continuations until the exact metainfo piece size is reached.
5. Overflow, missing or empty continuations, more than 1024 articles, and piece
   hash mismatch reject the entire chain.
6. If verification passes, the piece is written into the local torrent data
   path and marked locally complete.
7. Later peer retries are served from local disk.

If fetch, decode, write, or hash verification fails, the piece is not served.

## Usenet Piece Discovery

Discovery helps a node use Usenet data that another Nashawk node has already
uploaded for the same torrent.

After magnet metadata or torrent metainfo is available, Nashawk creates or
loads the torrent's Usenet manifest. If the manifest has no meaningful existing
piece state and discovery is enabled, Nashawk selects a deterministic bounded
sample of piece indexes and validates each sampled complete article chain with
`BODY` requests, exact metainfo size, and the BitTorrent piece hash. Discovery
does not assume the uploader used the current node's payload limit.

Outcomes:

- if every sampled chain validates, all pieces in the manifest are marked
  `available`, and the torrent becomes Usenet-serviceable without waiting for a
  normal full BitTorrent download;
- if any sampled article is missing, discovery records `missing` and the torrent
  continues through normal BitTorrent download/upload behavior;
- if the query errors, discovery records `error` for diagnostics and leaves
  normal BitTorrent behavior intact.

Discovery is a fast path, not a correctness dependency. Every Usenet restore
still yEnc-decodes the article and verifies the BitTorrent piece hash before the
data is served.

The manifest records discovery metadata:

```json
{
  "discovery": {
    "status": "available",
    "checked_at": 1783814490,
    "sample_size": 16,
    "sampled_pieces": [0, 9, 14]
  }
}
```

Discovery IO shares the same Usenet IO limit as upload and download work.

## Web UI And RPC Observability

The Web UI exposes a read-only Usenet status surface without exposing secrets.

Session-level state is shown in the Statistics dialog and includes:

- whether Usenet mode and eviction are enabled;
- configured Usenet IO limit and current active IO;
- upload/download queue sizes and in-flight download count;
- discovery settings.

Torrent-level state is shown in the torrent row and Inspector info tab. The UI
distinguishes local completion from Usenet-backed serviceability, including
cases where local pieces have been evicted but the torrent remains servable
through Usenet.

A torrent whose pieces are fully servable from Usenet can appear in the seeding
activity state even when local progress is below 100%. The Usenet badges and
Inspector summary show whether that state is local, mixed, or Usenet-backed.

The browser can emit stable diagnostic console messages with the
`[nashawk-usenet]` prefix when the local debug option is enabled. RPC and Web UI
diagnostics intentionally exclude Usenet host, port, username, password, group,
and `.env` contents.

## Version Compatibility Mode

For compatibility testing, Nashawk defaults to exposing Transmission
4.1.2-compatible external identifiers:

- peer ID prefix: `-TR4120-`
- peer extension handshake client string: `Transmission 4.1.2`
- HTTP User-Agent: `Transmission/4.1.2`
- RPC/session version: `4.1.2 (00000000)`

This does not change the internal build or codebase. To expose the real Nashawk
development version instead, start the daemon with:

```bash
--no-version-compat
```

The bundled helper scripts also accept:

```bash
NASHAWK_VERSION_COMPAT_ENABLED=0
```

## Deleting Local Data

After pieces have been uploaded and marked `available`, local data may be
removed either manually or by enabling the local eviction policy. A later verify
will show the torrent as locally incomplete, but the node can still advertise
and serve pieces backed by the Usenet manifest.

On demand, missing local pieces are restored from Usenet. Restored pieces remain
in the torrent data path until removed by an operator or by a later eviction
pass after the configured minimum age.

## Operational Notes

- Use this mode only with content you are allowed to store and distribute.
- Use a Usenet provider account that explicitly permits posting.
- Set `usenet_check_article_size` to the maximum raw payload for each Usenet
  article. Larger BitTorrent pieces are split automatically.
- Use `--usenet-upload-concurrency` conservatively and keep it at or below the
  provider account's simultaneous NNTP connection limit.
- Keep `usenet_eviction_enabled` off until the node has uploaded and sampled
  enough pieces to trust the Usenet manifest for the torrents being served.
- Keep `.env` permissions restrictive, for example `0600`.
- Back up `<config-dir>/usenet-pieces/`; without manifests, the node does not
  know which message-ids correspond to torrent pieces.

## Quick Local Validation

Build and run the normal test suite:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Start a Usenet-enabled daemon:

```sh
./build/daemon/transmission-daemon -f \
  -g /path/to/config \
  -w /path/to/downloads \
  -T -p 19191 -P 51441 --no-portmap \
  --usenet-enabled \
  --usenet-check-article-size 2097152 \
  --usenet-upload-concurrency 40 \
  --log-level=trace \
  -e /path/to/daemon.log
```

Add an eligible torrent whose data already exists locally:

```sh
./build/utils/transmission-remote 127.0.0.1:19191 \
  -a /path/to/file.torrent \
  -w /path/to/downloads
```

Watch for upload activity:

```sh
rg "Queued .*Usenet upload|Usenet upload completed|Usenet upload failed" /path/to/daemon.log
```

Check manifest status:

```sh
python3 - <<'PY'
import collections, json, pathlib
for path in pathlib.Path('/path/to/config/usenet-pieces').glob('*.json'):
    data = json.loads(path.read_text())
    print(path.name, collections.Counter(p.get('status') for p in data['pieces']))
PY
```

To validate restore behavior, delete the local data file, run verify, then
request pieces from a peer. Provider logs should show:

```text
Queued piece N for Usenet download
Usenet piece N restored to local data
```

## Troubleshooting

`Usenet mode requires nyuu in PATH`

Install `nyuu` and ensure the daemon environment can find it.

`Usenet mode requires nyuu's yencode module to load with the active Node.js runtime`

The daemon found `nyuu`, but the active `node` binary could not load that
Nyuu installation's native yEnc module. Reinstall or rebuild Nyuu under the same
Node.js version used to start the daemon, and make sure `PATH` resolves both
`node` and `nyuu` to the intended installation.

`Missing Usenet configuration`

Create `.env` or export the required `USENET_*` variables.

Authentication failure

Check username, password, TLS flag, host, and port. Keep quotes around shell
special characters in `.env` values.

Posting failure

Confirm that the provider account has posting enabled and that the selected
group accepts posts.

Article-size validation failure

Lower `usenet_check_article_size` to a raw payload the provider accepts. This
increases the number of articles per large piece; torrents requiring more than
1024 articles per piece remain ineligible.

Peer sees availability but first request is rejected

This can happen for a cold Usenet-only piece. The request queues an asynchronous
fetch; once the piece is restored, peer retries can be served.

## Related Documents

- [Active Usenet Multipart Piece Design](./Usenet-Multipart-Piece-Design.md)
- [Active Usenet Multipart Piece Implementation Plan](./Usenet-Multipart-Piece-Implementation-Plan.md)
- [Archived Usenet Piece Backend Design](./archive/Usenet-Piece-Backend.md)
- [Archived Usenet Piece Backend Implementation Plan](./archive/Usenet-Piece-Backend-Plan.md)
- [Archived Usenet Local Piece Eviction Plan](./archive/Usenet-Local-Piece-Eviction-Plan.md)
- [Archived Usenet Nyuu Batch Upload Plan](./archive/Usenet-Nyuu-Batch-Upload-Plan.md)
- [Archived Usenet Web UI Plan](./archive/Usenet-Web-UI-Plan.md)
- [Archived Usenet Piece Discovery Plan](./archive/Usenet-Piece-Discovery-Plan.md)
- [Archived Usenet Backend Test Status](./archive/Usenet-Backend-Test-Status-2026-07-11.md)
