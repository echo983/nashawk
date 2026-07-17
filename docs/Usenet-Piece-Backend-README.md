# Usenet Piece Backend README

Nashawk includes an experimental Usenet-backed piece storage mode for
`transmission-daemon`. The mode lets a node keep normal BitTorrent torrent
metadata while storing verified pieces as Usenet articles. This is intended for
hosts with limited local disk, such as small VPS instances, that still need to
serve torrents after local data has been removed.

Usenet mode is opt-in. When it is not enabled, Nashawk follows the normal
Transmission behavior.

## Status

This backend is functional but experimental. The implementation has been
validated with local two-daemon BitTorrent tests, real Usenet upload/read paths,
multipart articles, full integrity audits, repair, and verified local eviction.

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
- Web UI and RPC observability for daemon and torrent Usenet state
- evidence-driven and manually requested sampled Usenet piece discovery
- mandatory independent readback before an uploaded piece becomes evictable
- full torrent-level Usenet integrity audits with repair and BitTorrent fallback
- manual `Verify Usenet data` action in the Web UI torrent context menu
- manual `Discover on Usenet` action in the Web UI torrent context menu
- optional automatic full integrity audit before a Usenet Ready transition
- immediate eviction eligibility after mandatory remote readback succeeds
- startup validation through the same Nyuu upload and piece download path used
  by normal operation

The integrity repair milestone passed all 619 enabled CTest cases and repeated
real-provider startup upload/readback checks on 2026-07-17. Eleven upstream
tests remained disabled by the project's normal test configuration.

The evidence-driven discovery milestone passed all 626 enabled CTest cases, a
Web UI production build, and a real-provider duplicate-upload roundtrip on
2026-07-17. The provider test used a trackerless 1.5 MiB random fixture with six
256 KiB pieces. A first clean node uploaded and read back all six pieces. A
second clean manifest then received and verified three distinct duplicate
Message-ID responses, automatically queued discovery, validated all six sample
chains, and finished with discovery `available`, trigger
`duplicate_evidence`, and integrity `ready`. Eleven upstream tests remained
disabled by the normal project configuration.

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
- Automatic discovery requires verified duplicate-upload evidence and uses a
  persisted retry cooldown. Metainfo alone does not query Usenet. Operators can
  explicitly request discovery from the torrent context menu.
- End-to-end Usenet tests are currently manual, not CI automation.
- Windows startup support for this mode is not implemented.

Prioritized remaining work is tracked in the
[Usenet Backend Roadmap](Usenet-Backend-Roadmap.md).

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

Manifest version 3 also records `verified_at` per piece. `available` means the
piece may be attempted from Usenet, while a nonzero `verified_at` proves that a
complete `BODY` chain was decoded to the exact metainfo size and matched the
BitTorrent piece SHA-1. Only the latter is eligible for local eviction. Version
1 and 2 manifests remain readable, but their historical availability has no
trusted eviction credential until it is revalidated.

The torrent-level integrity state is one of `not_checked`, `checking`,
`repairing`, `ready`, `incomplete`, or `error`. A full audit reads and verifies
every piece. A failed remote piece is withdrawn from peer-facing availability;
if a verified local copy exists it is uploaded again, otherwise normal
BitTorrent acquisition is allowed to recover it before upload. A torrent is
shown as Usenet Ready only after every remote piece has a verification
credential.

On startup, interrupted `uploading` states are reset to `unknown`, and existing
local verified pieces are scanned so they can be queued again.

Interrupted integrity audits are reset to a retryable error. Existing torrents
that appear completely Usenet-serviceable but have no full audit result are
automatically queued for an audit. Operators can rerun the same audit at any
time from the torrent context menu with `Verify Usenet data`.

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
  "usenet_auto_integrity_audit_enabled": false,
  "usenet_check_article_size": 2097152,
  "usenet_cache_size_mib": 0,
  "usenet_evict_after_readback": false,
  "usenet_eviction_enabled": false,
  "usenet_eviction_min_age_minutes": 0,
  "usenet_discovery_enabled": true,
  "usenet_discovery_sample_size": 16,
  "usenet_upload_concurrency": 40
}
```

Automatic full-torrent Usenet audits are disabled by default because every
uploaded piece already requires an independent size and SHA-1 readback before
it becomes available or evictable. Enable the additional full audit with
`--usenet-auto-integrity-audit`; disable it explicitly with
`--no-usenet-auto-integrity-audit`. Manual `Verify Usenet data` remains
available regardless of this setting.

Local piece eviction is disabled by default. To enable immediate eviction after
mandatory remote verification:

```sh
transmission-daemon --usenet-enabled \
  --usenet-eviction-enabled \
  --usenet-evict-after-readback \
  --usenet-eviction-min-age-minutes 0 \
  --usenet-cache-size-mib 0
```

`usenet_cache_size_mib` and `usenet_eviction_min_age_minutes` both default to
`0`. By default, eviction still waits for a fully verified Ready torrent. With
`--usenet-evict-after-readback`, each independently hash-verified piece may be
released immediately without waiting for torrent-wide integrity readiness.
Use `--no-usenet-evict-after-readback` to retain the conservative gate. Set a
positive minimum age or cache size to retain a local hot layer.

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
loads the torrent's Usenet manifest, but does not query Usenet merely because
metainfo exists. Normal uploads collect evidence only when Nyuu reports an exact
`441 Message-ID is not unique` response and Nashawk then reads back the complete
article chain, verifies its exact metainfo size, and matches the BitTorrent
piece hash.

Automatic discovery starts after three distinct verified duplicate pieces, or
all pieces for torrents with fewer than three pieces, provided duplicate
evidence represents at least half of distinct completed upload attempts. A
failed automatic run consumes that evidence window and enforces a persisted
30-minute retry cooldown. The `Discover on Usenet` context action bypasses the
evidence threshold and cooldown.

Discovery selects a deterministic bounded sample and prefers pieces that did
not already contribute duplicate evidence. Each sampled complete article chain
is validated with `BODY` requests, exact metainfo size, and the BitTorrent piece
hash. Discovery does not assume the uploader used the current node's payload
limit.

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
    "trigger": "duplicate_evidence",
    "checked_at": 1783814490,
    "evidence_window_started_at": 1783814400,
    "retry_after": 0,
    "sample_size": 16,
    "sampled_pieces": [0, 9, 14],
    "attempted_pieces": [2, 4, 7],
    "duplicate_verified_pieces": [2, 4, 7]
  }
}
```

Discovery IO shares the same Usenet IO limit as upload and download work.

## Web UI And RPC Observability

The Web UI exposes Usenet status and bounded maintenance actions without
exposing secrets.

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

The torrent context menu provides `Discover on Usenet` for explicit bounded
sample discovery and `Verify Usenet data` for a full integrity audit and repair.
The torrent RPC summary includes the discovery trigger, evidence and attempt
counts, required threshold, evidence-window timestamp, and retry timestamp.

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
- RPC/session version: `4.1.2 (f234716f3e)`

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

- [Archived Usenet Multipart Piece Design](./archive/Usenet-Multipart-Piece-Design.md)
- [Archived Usenet Multipart Piece Implementation Plan](./archive/Usenet-Multipart-Piece-Implementation-Plan.md)
- [Archived Usenet Piece Backend Design](./archive/Usenet-Piece-Backend.md)
- [Archived Usenet Piece Backend Implementation Plan](./archive/Usenet-Piece-Backend-Plan.md)
- [Archived Usenet Local Piece Eviction Plan](./archive/Usenet-Local-Piece-Eviction-Plan.md)
- [Archived Usenet Nyuu Batch Upload Plan](./archive/Usenet-Nyuu-Batch-Upload-Plan.md)
- [Archived Usenet Web UI Plan](./archive/Usenet-Web-UI-Plan.md)
- [Archived Usenet Piece Discovery Plan](./archive/Usenet-Piece-Discovery-Plan.md)
- [Archived Usenet Backend Test Status](./archive/Usenet-Backend-Test-Status-2026-07-11.md)
