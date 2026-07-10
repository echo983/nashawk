# Usenet Piece Backend

Status: Archived.

This design document captured the initial Usenet-backed piece storage design.
The first implementation has been completed and validated. Use
`docs/Usenet-Piece-Backend-README.md` for current operator-facing behavior and
configuration. Remaining future work is tracked as follow-up implementation
work, not in this archived design.

This document describes the initial design for Nashawk's optional Usenet-backed piece storage mode.

## Goal

Nashawk should be able to run on hosts with limited local storage while still serving torrents. The node keeps normal torrent metadata and swarm behavior, but may store cold pieces in Usenet instead of on local disk.

This is not a general Usenet proxy. Nashawk only serves pieces for torrents it already manages locally, using the torrent's metadata, infohash, piece indexes, and piece hashes as the authority.

## Operating Mode

Usenet support is opt-in.

Initial switches:

- `usenet-enabled` in settings
- `--usenet-enabled` for `transmission-daemon`
- `usenet-upload-concurrency` in settings
- `--usenet-upload-concurrency <count>` for `transmission-daemon`

The shared Usenet upload/download IO limit defaults to 4 and is clamped to the local safety range 1-64. Operators can raise it to match their provider account, for example 40 concurrent Usenet operations on a server plan that permits 40 NNTP connections.

When enabled, startup must fail fast if the Usenet backend cannot be used. Required checks:

- `nyuu` is present and executable
- Usenet configuration is present in local environment or `.env`
- NNTP authentication succeeds
- Posting succeeds using a tiny yEnc loopback article
- Reading by `Message-ID` succeeds for the loopback article

Secrets must remain outside git. `.env` is ignored; `.env.example` documents required keys.

## Availability Model

Do not overload Transmission's existing local completion state.

Use three separate concepts:

- `local_has_piece`: the original Transmission meaning; the piece exists locally and has been verified
- `usenet_has_piece`: Nashawk has sampled or otherwise verified that the piece is present in Usenet
- `servable_has_piece`: the piece can be served to peers; this is `local_has_piece || usenet_has_piece`

Progress, resume data, local verification, and disk completion should continue to use `local_has_piece`. Peer-facing bitfields and request eligibility use `servable_has_piece`.

## Message Identity

Each Usenet-backed piece needs deterministic addressing.

Preferred `Message-ID` stem:

```text
nashawk-piece-{infohash}-{piece_index}-{piece_hash}
```

Full message id:

```text
<nashawk-piece-{infohash}-{piece_index}-{piece_hash}@nashawk.local>
```

The piece hash remains the main identity component, but including `infohash` and `piece_index` improves debugging and avoids accidental ambiguity.

## Article Layout

The primary storage model is one BitTorrent piece per Usenet article.

This matches the intended deployment target: modern paid binary Usenet providers with large article handling, high retention, and high completion. It also keeps the system simple:

- one piece hash maps to one `Message-ID`
- no NZB dependency is required
- lookup is deterministic
- readback fetches one article and verifies one piece hash

Canonical layout:

```text
piece hash -> article
```

Message body:

```text
yEnc(piece bytes)
```

The article headers include the torrent infohash, piece index, and piece hash for diagnostics, but the piece hash remains the lookup identity.

Provider limits are runtime facts, not protocol guarantees. Startup validation should post and read back a test article sized to the configured torrent piece policy. If a provider rejects that size, Usenet mode must fail fast for that configuration.

Nashawk never splits one BitTorrent piece across multiple Usenet articles. If a torrent's piece size cannot be represented as one article on the configured provider, that torrent is not eligible for Usenet-backed service and must be rejected for this mode.

The local Usenet piece manifest records:

- torrent infohash
- piece index
- piece hash
- piece size
- message ID
- verification state
- last checked time
- failure count

The upload path may use Nyuu to post a single article per piece. The read path should use direct NNTP `BODY <message-id>` fetches, then yEnc decode and verify the piece hash.

## Upload Flow

Hook point:

- `tr_torrent::on_piece_completed(piece)`

Flow:

1. The normal downloader writes blocks locally.
2. Transmission verifies the piece hash.
3. `on_piece_completed()` enqueues the piece for Usenet upload.
4. The Usenet worker reads the full piece from local disk.
5. The worker posts one yEnc article using Nyuu or an internal poster.
6. The worker reads back enough data to confirm availability.
7. The worker records `usenet_has_piece[piece] = true`.
8. Cache policy may later evict the local piece if it is cold.

Upload failure must not corrupt normal torrent state. It only leaves `usenet_has_piece[piece] = false` or `unknown`.

## Read Flow

Hook points:

- local read path: `tr_ioRead()`
- peer request response path: `tr_peerMsgsImpl::add_next_block()`

Flow:

1. A block is needed for a piece.
2. If `local_has_piece` is true, use the existing `tr_ioRead()` path.
3. If local data is missing but `usenet_has_piece` is true:
   - check the local piece cache
   - if missing, fetch the article by `Message-ID`
   - yEnc decode the piece
   - verify the piece hash against torrent metadata
   - place the piece in the local cache
   - serve the requested block from cache
4. If Usenet fetch fails or hash verification fails:
   - mark the Usenet piece unavailable
   - retract peer-facing availability for that piece
   - handle the request as unavailable

The peer IO path must not block on NNTP. A request for an uncached Usenet-only piece should trigger asynchronous fetch. The current request can be rejected or delayed until the cache has the piece.

## Peer-Facing Availability

In Usenet mode, peer-facing piece availability uses:

```text
servable_bitfield = local_completion_bitfield OR usenet_available_bitfield
```

Affected behavior:

- initial bitfield sent to peers
- HAVE messages after Usenet availability changes
- request validation for pieces we claim to have
- stats and diagnostics that distinguish local versus Usenet-backed service

Internal local completion should not be rewritten simply because a piece exists in Usenet.

## Sampling And Verification

Full verification of all Usenet-backed pieces can be expensive, so Nashawk uses practical sampling.

Suggested checks:

- on torrent load, sample a bounded number of Usenet-backed pieces
- periodically sample random known Usenet pieces
- before declaring many pieces to peers, sample enough to establish confidence
- on demand, fetch and verify before serving a cold piece
- on any fetch or hash failure, immediately downgrade that piece

States:

- `unknown`: no current confidence
- `available`: sampled or uploaded and read back successfully
- `failed`: recent read or hash failure

Only `available` contributes to `servable_has_piece`.

## Local Cache

Usenet mode needs a bounded local piece cache.

Initial settings:

- `usenet-cache-dir`
- `usenet-cache-size-mib`
- `usenet-cache-ttl-minutes`

Policy:

- cache full verified pieces, not arbitrary blocks
- pin pieces currently serving peer requests
- use LRU for eviction
- preserve hot pieces
- never evict a piece while it is being uploaded, fetched, or verified

The original local data files and the Usenet cache must be kept conceptually separate so normal Transmission completion remains meaningful.

## Implementation Phases

### Phase 1: Service Skeleton

- Add settings and daemon switch.
- Add `UsenetService`.
- Load local `.env` or environment variables.
- Validate `nyuu`.
- Validate NNTP auth.
- Validate post/read loopback.
- Add `.env.example` and ignore `.env`.

### Phase 2: Upload Pipeline

- Connect `on_piece_completed()` to a bounded upload queue.
- Read complete pieces from local disk.
- Upload via Nyuu.
- Persist manifest entries.
- Mark `usenet_has_piece` only after readback succeeds.

### Phase 3: Read Pipeline

- Implement direct NNTP fetch by message ID.
- Implement yEnc decode for a single-piece article.
- Verify piece hash before use.
- Store verified pieces in cache.

### Phase 4: Servable Availability

- Introduce `servable_has_piece`.
- Build peer-facing bitfields from local plus Usenet availability.
- Emit HAVE when a Usenet piece becomes available.
- Retract or stop advertising failed pieces where protocol allows.

### Phase 5: Cache And Sampling

- Add cache size enforcement.
- Add periodic sampling.
- Add failure backoff.
- Add observability in logs and RPC.

## Feasibility Notes

The design is feasible, but only if these constraints are respected:

- Usenet-only service must be asynchronous; blocking NNTP inside peer IO is not acceptable.
- The design requires one piece per article. This is a deployment contract that must be validated against the configured provider and torrent piece size at startup and per torrent.
- Multi-article piece storage is explicitly out of scope. Torrents whose piece size cannot fit in one provider article are rejected for Usenet mode.
- Hash verification is mandatory before declaring or serving fetched data.
- Existing local completion semantics must remain intact.
- BitTorrent v2 support should initially be treated conservatively. The current code still has v2 TODOs, so the first implementation should target v1 and hybrid torrents whose v1 piece hashes are available through `piece_hash()`.
- Newly posted articles may not be immediately readable from all servers. Startup and upload checks should read from the configured server, while runtime sampling handles later inconsistencies.

## Open Questions

- Should the first implementation use Nyuu for upload only, or replace it with an internal NNTP poster after the prototype?
- Should the manifest be stored under `resume/`, a new `usenet-pieces/` directory, or embedded in torrent resume data?
- What is the first safe policy for declaring Usenet-only pieces: declare all sampled available pieces, or ramp up declaration gradually per torrent?
- Should local eviction remove data from original torrent files, or should the first version only use Usenet as an additional backing store without sparse-file reclamation?
