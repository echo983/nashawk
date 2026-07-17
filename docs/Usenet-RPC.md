# Nashawk Usenet RPC

Nashawk extends Transmission's JSON-RPC 2.0 `torrent_get` response with the
read-only `usenet_piece_summary` field. Automation such as a private control
service or Cloudflare Worker can use this field to observe logical piece upload,
discovery, serviceability, and integrity state.

The RPC endpoint is normally:

```text
POST http://host:9091/transmission/rpc
```

Clients must implement Transmission's `X-Transmission-Session-Id` handshake:
retry an HTTP 409 response with the token returned in that response header.

## Request

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "torrent_get",
  "params": {
    "ids": [42],
    "fields": ["id", "name", "hash_string", "usenet_piece_summary"]
  }
}
```

Omit `ids` to query every torrent. Polling clients should request only the
fields they consume. A five-second polling interval is suitable for operator
status; faster polling does not make uploads complete sooner.

## Response

Each object in `result.torrents` may contain:

```json
{
  "id": 42,
  "name": "example",
  "hash_string": "0123456789abcdef0123456789abcdef01234567",
  "usenet_piece_summary": {
    "eligible": true,
    "manifest_present": true,
    "piece_count": 525,
    "local_piece_count": 260,
    "unknown": 0,
    "uploading": 203,
    "available": 321,
    "verified": 321,
    "failed": 1,
    "servable": 525,
    "discovery": {
      "status": "not_checked",
      "trigger": "none",
      "checked_at": 0,
      "evidence_window_started_at": 0,
      "retry_after": 0,
      "attempted_piece_count": 0,
      "duplicate_evidence_count": 0,
      "required_evidence_count": 3,
      "sample_size": 0,
      "sampled_pieces": []
    },
    "integrity": {
      "status": "not_checked",
      "started_at": 0,
      "finished_at": 0,
      "checked": 0,
      "verified": 0,
      "missing": 0,
      "repairing": 0,
      "waiting_for_peers": 0
    }
  }
}
```

Counts describe logical BitTorrent pieces, not multipart Usenet articles.

| Field | Meaning |
|---|---|
| `eligible` | The torrent can use the Usenet backend under current limits. |
| `manifest_present` | Nashawk has a persistent Usenet piece manifest. |
| `piece_count` | Total logical pieces in the torrent. |
| `local_piece_count` | Pieces currently verified in local storage. |
| `unknown` | Pieces with no confirmed Usenet state. |
| `uploading` | Pieces queued, staged, uploading, or awaiting readback confirmation. |
| `available` | Pieces whose complete article chain passed readback confirmation. |
| `verified` | Available pieces confirmed by upload readback, discovery, or integrity verification. |
| `failed` | Pieces whose latest upload or readback attempt failed. |
| `servable` | Pieces currently obtainable from either verified local storage or Usenet. |

`error` may be present in `discovery` or `integrity` when the corresponding job
failed. Consumers must tolerate additional fields and unknown status strings so
that Nashawk can extend diagnostics without breaking them.

## Derived States

The Web UI labels are presentation states and are not returned by RPC. Compute
automation states from counters:

```js
const summary = torrent.usenet_piece_summary
const hasSummary = summary?.eligible && summary.manifest_present && summary.piece_count > 0
const uploadProgress = hasSummary ? summary.available / summary.piece_count : 0
const fullyServable = hasSummary && summary.servable >= summary.piece_count
const usenetComplete =
  hasSummary &&
  summary.available >= summary.piece_count &&
  summary.uploading === 0 &&
  summary.failed === 0
const needsAttention = hasSummary && summary.uploading === 0 && summary.failed > 0
```

`fullyServable` and `usenetComplete` are deliberately different:

- `fullyServable` may be true while pieces are still uploading because local
  copies can serve them.
- `usenetComplete` means every logical piece is confirmed on Usenet.
- `failed > 0` is not completion. Keep local data and run integrity repair.
- `local_piece_count === 0` is not required for upload completion; it only means
  the local cache has been fully evicted.

Progress is piece-based rather than byte-based. Torrents normally use a fixed
piece size, but the final piece may be shorter.

## Security Boundary

Do not expose Transmission RPC directly to an untrusted browser or the public
Internet. `X-Transmission-Session-Id` is CSRF protection, not authentication.
Use RPC authentication plus a private tunnel, Cloudflare Access, or a small
authenticated server-side API that returns only required status fields.

Never put Usenet credentials in Worker responses or browser code. Nashawk does
not expose Usenet host, username, password, group, or `.env` contents through
this summary.
