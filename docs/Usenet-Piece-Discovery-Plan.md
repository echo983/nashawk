# Usenet Piece Discovery Plan

Status: initial design for `feature/usenet-piece-discovery`.

## Goal

When a user adds a magnet or torrent whose pieces were already uploaded to
Usenet by another Nashawk node, the local client should be able to discover
that fact after metainfo is available and use Usenet as the primary backing
store. If discovery fails or later piece readback fails, the torrent falls back
to normal BitTorrent download behavior.

The intended user-visible result is:

- add magnet;
- wait for metadata;
- sample Usenet for Nashawk message IDs derived from piece hashes;
- if every sample exists, mark the torrent as Usenet-backed and serviceable;
- fetch missing local pieces from Usenet on demand;
- let BitTorrent fill any later missing or failed pieces.

## Non-Goals

- Do not search arbitrary Usenet content or NZB indexes.
- Do not proxy a global Usenet piece universe.
- Do not split one BitTorrent piece across multiple Usenet articles.
- Do not do full-torrent preflight checks by default.
- Do not block normal BitTorrent operation while discovery is running.

## Existing Hooks

The current code already has most of the required plumbing:

- `tr_torrent::set_metainfo()` calls `session->ensureUsenetTorrent(this)` after
  magnet metadata is completed.
- `tr_session::ensureUsenetTorrent()` creates or validates the local Usenet
  manifest.
- `tr_session::hasUsenetPiece()` treats manifest entries in `available` state
  as locally serviceable pieces.
- peer request handling already calls `fetchUsenetPiece()` when a requested
  piece is Usenet-available but not local.
- `tr_usenet_piece_store` already derives message IDs from piece hashes as
  `<piece_hash>@nashawk.local`.
- `NntpConnection` already supports connect, auth, group, and BODY. It can be
  extended with a lightweight `STAT <message-id>` command.

This means the discovery feature can be implemented as a manifest-state
producer, not as a rewrite of the BitTorrent picker or peer protocol.

## First-Version Behavior

Discovery runs only when all of these are true:

- Usenet mode is enabled.
- Usenet discovery is enabled.
- Torrent has metainfo.
- Torrent piece size is eligible for the configured max article size.
- Local manifest exists and has no locally known available/uploading/failed
  state, or the manifest does not exist yet.
- Discovery has not already completed or failed for this torrent in the current
  manifest.

The first implementation should be conservative: a torrent that already has
local Usenet state should not be overwritten by discovery.

## Sampling

Discovery should sample a small deterministic subset of piece message IDs:

- always include piece `0`;
- include the last piece;
- include the middle piece when the torrent has at least three pieces;
- add deterministic pseudo-random pieces derived from the infohash until the
  configured sample count is reached;
- de-duplicate indices.

Suggested defaults:

```text
usenet_discovery_enabled: true
usenet_discovery_sample_size: 16
usenet_discovery_max_in_flight: 1
```

Discovery is on by default whenever Usenet mode is enabled. Operators can
disable it when they want pure local-download-backed Usenet population.

For tiny torrents, sample count is capped at `piece_count`.

## Usenet Query

Add a lightweight existence API:

```cpp
struct tr_usenet_article_exists_request
{
    std::string_view config_dir;
    std::string_view message_id;
};

enum class tr_usenet_article_exists_result
{
    Exists,
    Missing,
};

std::variant<tr_usenet_article_exists_result, std::string>
tr_usenet_article_exists(tr_usenet_article_exists_request const& request);
```

Implementation:

- load `.env`/settings through the existing Usenet config path;
- connect/auth/group exactly like download;
- issue `STAT <message-id>`;
- treat `223` as exists;
- treat `430` as missing;
- treat transport/auth/group and unexpected codes as errors.

Errors should fail discovery for that attempt but should not stop the torrent.

## Manifest Update

If every sampled article exists:

- ensure the manifest exists;
- mark every piece as `available`;
- preserve the deterministic message IDs already generated from piece hashes;
- write a discovery marker in the manifest so the UI/RPC can distinguish
  discovered Usenet-backed state from locally uploaded state.

Suggested manifest metadata additions:

```json
"discovery": {
  "status": "available",
  "checked_at": 1783797000,
  "sample_size": 16,
  "sampled_pieces": [0, 145, 290],
  "source": "stat-sample"
}
```

If any sampled article is missing:

- keep or create the manifest with pieces still `unknown`;
- record discovery status as `missing`;
- let normal BitTorrent download proceed.

If discovery errors:

- record discovery status as `error`;
- keep pieces `unknown`;
- let normal BitTorrent download proceed.

This is intentionally optimistic after a full sample pass. Actual BODY fetches
still verify yEnc decode size/checksum and will mark bad pieces unavailable.

## Runtime Fallback

Even after discovery marks a torrent available, any individual Usenet BODY
download can fail. Existing behavior should remain:

- failed Usenet read marks that piece non-available or failed;
- `hasUsenetPiece()` stops advertising it as available;
- BitTorrent can request/download that piece normally;
- after local completion, normal `onUsenetPieceCompleted()` can upload it back
  to Usenet and mark it available again.

This makes discovery a fast path, not a correctness dependency.

## RPC and Web UI

Expose discovery state in the existing `usenet_piece_summary` object:

```json
"discovery": {
  "status": "not_checked|checking|available|missing|error",
  "sample_size": 16,
  "checked_at": 1783797000,
  "error": "..."
}
```

The Web UI can then refine badges:

- `Usenet Ready`: all pieces Usenet-available, including discovered state.
- `Discovered`: all pieces available due to sample discovery, before any local
  BODY fetch has been observed.
- `Mixed Ready`: existing meaning, local plus Usenet-backed pieces cover all
  pieces.
- `Incomplete`: neither local nor Usenet-backed state covers the torrent.

The first implementation can expose RPC only and defer badge refinements if
needed.

## Configuration

Add settings and CLI flags:

```text
--usenet-discovery-enabled
--no-usenet-discovery
--usenet-discovery-sample-size <N>
```

Initial defaults:

```text
usenet_discovery_enabled: true
usenet_discovery_sample_size: 16
```

The local test script enables discovery by default. Disable it explicitly:

```sh
NASHAWK_USENET_DISCOVERY_ENABLED=0 ./scripts/run-usenet-webui-test.sh
```

## Concurrency and Provider Friendliness

Discovery is intentionally low-volume:

- at most one torrent discovery in flight by default;
- at most one NNTP STAT connection per discovery worker initially;
- sample size bounded by configuration;
- no periodic rediscovery loop in v1;
- no full-torrent scan unless explicitly requested in a later feature.

All discovery IO must share the existing Usenet IO slot budget so upload,
download, and discovery cannot exceed the configured provider connection limit.

## Implementation Steps

1. Add manifest discovery metadata fields and parser/writer support.
2. Add deterministic sample selection helper with unit tests.
3. Add `NntpConnection::stat()` and public `tr_usenet_article_exists()`.
4. Add settings/CLI/RPC plumbing for discovery enablement and sample size.
5. Add a discovery worker in `tr_session` that:
   - triggers from `ensureUsenetTorrent()` after metainfo is ready;
   - skips torrents with meaningful existing Usenet state;
   - samples article existence;
   - writes discovery status and manifest states.
6. Add RPC summary fields for discovery state.
7. Add focused tests:
   - sample selection is deterministic and bounded;
   - manifest discovery metadata roundtrips;
   - discovery marks all pieces available only when all samples exist;
   - missing/error samples do not mark pieces available.
8. Run a field test:
   - use a torrent already uploaded to Usenet;
   - remove only the local manifest for that torrent;
   - re-add the magnet;
   - after metadata completes, confirm it becomes Usenet-ready without normal
     BitTorrent piece download.

## Open Questions

- Should failed discovery be retried automatically after a cooldown, or only
  manually via a future RPC/Web UI command?
- Should `available` discovered pieces use a distinct state, or should
  discovery metadata be enough? The lower-impact choice is metadata only.
- Should failed discovery be retried manually via RPC/Web UI in v1, or wait for
  a later refresh feature?
- Should sample count scale with torrent size beyond the fixed cap? The first
  version should use the fixed cap to keep provider load predictable.

## Recommended First Cut

Keep v1 small:

- default-on discovery flag with an explicit off switch;
- sample size default 16;
- one discovery at a time;
- STAT-only probes;
- all-samples-pass marks full manifest available;
- failed/missing/error falls back to normal BT;
- no manual refresh UI yet.

This should deliver the desired behavior while keeping the blast radius mostly
inside Usenet service, manifest storage, settings, and `ensureUsenetTorrent()`.
