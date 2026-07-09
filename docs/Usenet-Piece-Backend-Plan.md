# Usenet Piece Backend Implementation Plan

This plan breaks the Usenet piece backend into small milestones. The design constraints are documented in `docs/Usenet-Piece-Backend.md`.

## Guiding Constraints

- Usenet mode is opt-in.
- Secrets stay local and must not be logged or committed.
- `1 BitTorrent piece = 1 Usenet article` is mandatory.
- Nashawk never splits a BitTorrent piece across multiple Usenet articles.
- Torrents whose piece size cannot fit in one provider article are rejected for Usenet-backed service.
- Local completion state remains the original Transmission state.
- Peer-facing serviceability is `local_has_piece || usenet_has_piece`.
- NNTP network work must not block peer IO paths.

## M0: Safety And Configuration

Goal: make Usenet mode configurable without exposing credentials.

Tasks:

- Keep `.env` ignored.
- Keep `.env.example` as the documented local template.
- Add daemon setting `usenet-enabled`.
- Add daemon flag `--usenet-enabled`.
- Add settings for:
  - `usenet-cache-dir`
  - `usenet-cache-size-mib`
  - `usenet-check-article-size`
- Load credentials from environment or `.env`.
- Avoid including Usenet username, password, server host, or generated temporary config in logs.

Acceptance:

- Starting without `usenet-enabled` behaves exactly as upstream Transmission.
- Starting with `usenet-enabled` but missing config fails with a clear non-secret error.
- `.env` remains ignored by git.

## M1: UsenetService Startup Validation

Goal: fail fast if Usenet cannot be used.

Tasks:

- Add a `UsenetService` owned by `tr_session` when Usenet mode is enabled.
- Check that `nyuu` exists and is executable.
- Parse Usenet configuration:
  - host
  - port
  - TLS flag
  - username
  - password
  - from
  - group
- Perform direct NNTP auth check.
- Perform tiny yEnc post/read loopback.
- Perform configured article-size post/read check.
- Store the confirmed provider article size capability in memory for torrent admission checks.

Acceptance:

- Valid credentials and posting capability let the daemon start.
- Invalid credentials stop startup.
- No `nyuu` stops startup.
- Posting disabled stops startup.
- Provider rejecting the configured article-size check stops startup.

## M2: Manifest And Usenet State

Goal: persist Usenet piece availability separately from local completion.

Tasks:

- Add a manifest directory, initially:
  - `<config-dir>/usenet-pieces/`
- Store one manifest file per torrent infohash.
- Manifest entry fields:
  - infohash
  - piece index
  - piece hash
  - piece size
  - message ID
  - state: `unknown`, `uploading`, `available`, `failed`
  - last checked timestamp
  - failure count
- Add an in-memory `usenet_available_bitfield`.
- Add deterministic message ID generation:

```text
<nashawk-piece-{infohash}-{piece_index}-{piece_hash}@nashawk.local>
```

Acceptance:

- Manifest loads on torrent startup.
- Manifest saves atomically after state changes.
- Local completion and Usenet availability can diverge without corrupting each other.

## M3: Torrent Admission

Goal: reject torrents that cannot satisfy the one-piece-one-article rule.

Tasks:

- On torrent add/start under Usenet mode, compare torrent piece size with confirmed provider article size.
- Reject Usenet-backed service for torrents with too-large pieces.
- Preserve normal non-Usenet behavior when Usenet mode is disabled.
- Add clear error text when a torrent is rejected for Usenet mode.

Acceptance:

- Eligible torrent proceeds.
- Ineligible torrent fails for Usenet mode before advertising Usenet-backed availability.
- No piece splitting path exists.

## M4: Upload Pipeline

Goal: upload verified local pieces to Usenet.

Tasks:

- Hook `tr_torrent::on_piece_completed(piece)`.
- Add a bounded upload queue.
- Skip pieces already marked `available` or `uploading`.
- Read the complete piece from local disk.
- Generate a temporary Nyuu config with `0600` permissions.
- Upload one yEnc article through Nyuu.
- Read the article back by message ID.
- Decode yEnc and verify the piece hash.
- Mark the piece `available` only after readback verification.
- Mark failures with failure count and timestamp.
- Delete temporary Nyuu configs and NZB files.

Acceptance:

- Completing a piece creates one Usenet article.
- Upload success persists manifest state.
- Upload failure does not affect normal torrent completion.
- Secrets never appear in command-line args, logs, or manifest.

## M5: Read Pipeline

Goal: retrieve one Usenet-backed piece and verify it before use.

Tasks:

- Implement direct NNTP `BODY <message-id>` fetch.
- Implement yEnc decode for a single article.
- Verify decoded bytes against torrent `piece_hash(piece)`.
- Return verified full-piece bytes to callers.
- Downgrade manifest state to `failed` on missing article, decode error, or hash mismatch.

Acceptance:

- A known uploaded piece can be fetched and verified.
- Missing article is reported as unavailable.
- Hash mismatch is never served.

## M6: Local Piece Cache

Goal: keep recently fetched Usenet pieces locally without unbounded disk growth.

Tasks:

- Add cache directory.
- Store verified full pieces by infohash and piece index.
- Pin pieces currently serving peer requests.
- Add size accounting.
- Add LRU eviction.
- Keep cache separate from original torrent data files.

Acceptance:

- Cache hit avoids NNTP fetch.
- Cache miss fetches and stores.
- Cache size does not grow beyond configured limit.
- Local torrent completion remains unchanged by cache entries.

## M7: Servable Availability

Goal: expose Usenet-backed pieces to peers safely.

Tasks:

- Add `servable_has_piece(piece)` as local-or-Usenet availability.
- Build peer-facing bitfields from servable availability.
- Emit HAVE when a Usenet piece transitions to `available`.
- Stop advertising or reject requests when a piece transitions to `failed`.
- Keep local completion, progress, and resume semantics unchanged.

Acceptance:

- Local-only behavior is unchanged with Usenet disabled.
- With Usenet enabled, available Usenet pieces appear in peer-facing bitfields.
- Unknown and failed Usenet pieces are not advertised.

## M8: Peer Request Integration

Goal: serve Usenet-only pieces to peers without blocking peer IO.

Tasks:

- In the peer request path, use local reads for local pieces.
- For Usenet-only pieces:
  - check cache
  - if cached, serve requested block
  - if not cached, enqueue async fetch
  - reject or delay the current request according to protocol support
- After fetch success, future requests for that piece hit cache.
- After fetch failure, downgrade availability and reject requests.

Acceptance:

- Peer IO does not synchronously wait on NNTP.
- Cached Usenet pieces can be served as normal blocks.
- Failed Usenet fetch causes graceful degradation.

## M9: Sampling And Maintenance

Goal: keep Usenet availability honest over time.

Tasks:

- Sample bounded pieces on torrent load.
- Periodically sample random available pieces.
- Sample before large-scale declaration if manifest is stale.
- Back off failed pieces.
- Log state transitions without secrets.

Acceptance:

- Stale or missing articles are eventually downgraded.
- Sampling load is bounded.
- Usenet availability survives daemon restart through manifest state.

## M10: Tests

Goal: cover the new behavior at unit and integration levels.

Unit tests:

- dotenv parsing
- message ID generation
- manifest load/save
- state transitions
- yEnc encode/decode
- cache accounting and eviction
- torrent admission by piece size

Integration tests:

- direct NNTP auth check
- tiny post/read loopback
- configured article-size post/read check
- upload one piece and read it back
- cache fetch path

End-to-end tests:

- small torrent uploads completed pieces to Usenet
- restart loads manifest and declares available pieces
- delete local piece data, fetch from Usenet, verify hash, serve block

## First Implementation Cut

The first useful cut should stop at M5:

1. Config and startup validation.
2. Manifest state.
3. Torrent admission.
4. Upload verified pieces to Usenet.
5. Read and verify pieces from Usenet.

Do not connect peer-facing bitfields in the first cut. That reduces risk while proving the storage backend works.

## Open Decisions

- Whether Nyuu remains the long-term upload path or is replaced by an internal NNTP poster.
- Whether article-size capability is global per daemon or tracked per provider profile.
- Whether failed Usenet mode should stop the whole daemon or only disable Usenet service after startup.
- Whether local cold-piece eviction should modify original torrent files or only operate in a separate cache during early versions.
