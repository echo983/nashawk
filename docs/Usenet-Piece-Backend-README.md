# Usenet Piece Backend README

Nashawk includes an experimental Usenet-backed piece storage mode for
`transmission-daemon`. The mode lets a node keep normal BitTorrent torrent
metadata while storing verified pieces as Usenet articles. This is intended for
hosts with limited local disk, such as small VPS instances, that still need to
serve torrents after local data has been removed.

Usenet mode is opt-in. When it is not enabled, Nashawk follows the normal
Transmission behavior.

## Status

This backend is functional but experimental.

Validated paths:

- startup validation for `nyuu`, Usenet credentials, posting, and readback
- upload of completed local pieces to Usenet
- persisted per-torrent Usenet piece manifests
- peer-facing availability from local pieces plus Usenet-available pieces
- on-demand Usenet fetch when a peer requests a missing local piece
- yEnc decode and piece hash verification before recovered data is served
- recovery after daemon restart for interrupted uploads
- configurable shared Usenet IO limit

Current limits:

- Usenet downloads use one worker and share the configured Usenet IO limit with
  uploads.
- Restored pieces are written back into the torrent data path and are not yet
  managed by a bounded cache eviction policy.
- The first request for a cold Usenet-only piece can be rejected while the
  piece is fetched; peers should retry.
- End-to-end Usenet tests are currently manual, not CI automation.
- Windows startup support for this mode is not implemented.

## Storage Model

Nashawk uses one BitTorrent piece per Usenet article.

The backend does not split a BitTorrent piece across multiple Usenet articles.
If a torrent's piece size is larger than the configured article-size check, that
torrent is not eligible for this mode.

Each piece is addressed by a deterministic `Message-ID`. In the current
implementation, the message-id local part is the piece SHA1 hash:

```text
<piece-sha1@nashawk.local>
```

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
still use local completion.

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
transmission-daemon --usenet-enabled --usenet-check-article-size 262144
```

Configure the shared Usenet upload/download IO limit:

```sh
transmission-daemon --usenet-enabled --usenet-upload-concurrency 40
```

The shared Usenet IO limit defaults to 4 and is clamped to 1 through 64. Upload
workers and the download worker must acquire a slot from this same limit before
opening Usenet IO, so a setting of 40 will not create more than 40 concurrent
Usenet operations.

The equivalent settings keys are:

```json
{
  "usenet_enabled": true,
  "usenet_check_article_size": 262144,
  "usenet_upload_concurrency": 40
}
```

## Startup Validation

When `usenet_enabled` is true, daemon startup fails if any required backend
check fails.

Startup checks:

- `nyuu` is present in `PATH`
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
4. A temporary `nyuu` config is written with restrictive permissions.
5. `nyuu` posts one yEnc article using the deterministic message-id.
6. On success, the manifest entry becomes `available`.
7. On failure, the manifest entry becomes `failed`.
8. Temporary piece files and temporary `nyuu` configs are removed.

Upload failure does not change normal torrent completion state.

## Read And Serving Flow

When a peer requests a piece:

1. If the piece exists locally, Nashawk serves it through the normal path.
2. If it does not exist locally but the manifest says it is `available`,
   Nashawk queues an asynchronous Usenet fetch.
3. The current peer request can be rejected while the fetch is in flight.
4. The download worker fetches `BODY <message-id>` from Usenet.
5. The worker decodes yEnc and verifies the torrent piece hash.
6. If verification passes, the piece is written into the local torrent data
   path and marked locally complete.
7. Later peer retries are served from local disk.

If fetch, decode, write, or hash verification fails, the piece is not served.

## Deleting Local Data

After pieces have been uploaded and marked `available`, the local data file may
be removed. A later verify will show the torrent as locally incomplete, but the
node can still advertise and serve pieces backed by the Usenet manifest.

On demand, missing local pieces are restored from Usenet. Restored pieces remain
in the torrent data path until removed by an operator or a future cache policy.

## Operational Notes

- Use this mode only with content you are allowed to store and distribute.
- Use a Usenet provider account that explicitly permits posting.
- Set `usenet_check_article_size` to the maximum BitTorrent piece size you
  intend this node to support.
- Use `--usenet-upload-concurrency` conservatively and keep it at or below the
  provider account's simultaneous NNTP connection limit.
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
  --usenet-check-article-size 262144 \
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

`Missing Usenet configuration`

Create `.env` or export the required `USENET_*` variables.

Authentication failure

Check username, password, TLS flag, host, and port. Keep quotes around shell
special characters in `.env` values.

Posting failure

Confirm that the provider account has posting enabled and that the selected
group accepts posts.

Article-size validation failure

Lower `usenet_check_article_size` or reject torrents whose piece size is too
large for the provider.

Peer sees availability but first request is rejected

This can happen for a cold Usenet-only piece. The request queues an asynchronous
fetch; once the piece is restored, peer retries can be served.

## Related Documents

- [Usenet Piece Backend Design](./Usenet-Piece-Backend.md)
- [Usenet Piece Backend Implementation Plan](./Usenet-Piece-Backend-Plan.md)
