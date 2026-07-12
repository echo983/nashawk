# Usenet Local Piece Eviction Plan

Status: Archived.

This plan has been implemented for the first conservative local piece eviction
pass. The current implementation can evict Usenet-available local pieces with
hole punching, preserve peer-facing Usenet availability, and restore evicted
pieces from Usenet on peer demand. Use `../Usenet-Piece-Backend-README.md`
for current operator-facing configuration and limits.

Known follow-up work: cross-file piece eviction, full size-pressure/LRU policy,
UI controls, and automated end-to-end Usenet tests.

This plan defines the first automatic local-piece eviction pass for Nashawk's
Usenet-backed piece storage mode.

## Goal

Usenet mode should be able to run on storage-constrained hosts without manual
operator cleanup.

The target loop is:

1. A torrent piece is downloaded or already exists locally.
2. The piece is uploaded to Usenet and marked `available` in the manifest.
3. Nashawk may evict the local copy after conservative safety checks.
4. The node still advertises the piece as servable through Usenet availability.
5. When a peer later requests the piece, Nashawk fetches it from Usenet,
   verifies it, writes it back locally, and serves the retry.

This is not a general filesystem cleanup tool. It only handles pieces for
torrents managed by Nashawk, and only when Usenet mode explicitly says those
pieces are available.

## Non-Goals

- Do not delete arbitrary files outside torrent data paths.
- Do not delete pieces whose Usenet manifest state is not `available`.
- Do not split pieces or change the one-piece-one-article storage rule.
- Do not implement a separate cache directory in this first pass.
- Do not change default behavior for normal non-Usenet sessions.
- Do not add Web UI controls in this phase.

## Configuration

All settings default to conservative values.

Settings:

- `usenet_eviction_enabled`: default `false`
- `usenet_eviction_min_age_minutes`: default `60`
- `usenet_cache_size_mib`: default `0`

Meaning:

- If `usenet_eviction_enabled` is false, no automatic local piece eviction runs.
- `usenet_eviction_min_age_minutes` is the minimum age after a piece becomes
  Usenet `available` before it may be evicted.
- `usenet_cache_size_mib` is the target maximum local Usenet-restorable piece
  footprint. `0` means do not use size-pressure eviction, but age-based
  eviction may still run when enabled.

The existing `usenet_upload_concurrency` setting remains the shared Usenet IO
limit and is unrelated to local disk eviction.

## State Model

Existing state remains authoritative:

- local completion state means verified local data exists
- manifest `available` means Usenet is expected to have the piece
- peer serviceability is local availability OR manifest availability

The eviction pass changes local completion state only after it has safely
punched or removed local data for that piece.

The first pass should also persist enough metadata to make conservative choices
after restart. The minimum useful fields are:

- time when a piece became `available`
- time when a piece was last restored from Usenet or completed locally

If these timestamps are absent in an existing manifest, the implementation must
treat the piece as recently available and avoid immediate eviction.

## Candidate Rules

A piece is eligible for eviction only when all conditions are true:

- Usenet mode is enabled.
- `usenet_eviction_enabled` is true.
- The torrent has metainfo.
- The torrent has the piece locally.
- The manifest exists.
- The manifest entry for that piece is `available`.
- The piece size is still eligible for the configured Usenet article limit.
- The piece has been `available` for at least
  `usenet_eviction_min_age_minutes`.
- The piece is not currently uploading to Usenet.
- The piece is not currently downloading from Usenet.
- The piece is not in a verifier-critical path.
- The piece is not being actively served or recently requested.

First-pass active-use detection may be coarse. If the implementation cannot
prove a piece is idle, it should skip eviction.

## Eviction Mechanics

The first implementation should prefer a simple and robust mechanism:

1. Select candidate pieces.
2. For each candidate, remove local bytes for the piece from the torrent data
   path.
3. Mark local completion false for that piece.
4. Mark the torrent dirty so resume state is updated.
5. Keep the manifest entry `available`.
6. Emit a non-secret trace or info log with torrent name and piece index.

For single-file torrents and most sparse-file-capable filesystems, hole punching
is the preferred low-risk mechanism. If hole punching is not available on a
platform or file layout, the first implementation may skip eviction instead of
rewriting large files.

The implementation must never mark a piece as locally missing unless the local
data was actually removed or deallocated successfully.

## Scheduling

Use a session-owned timer when Usenet mode is enabled.

Suggested behavior:

- Run shortly after startup to handle existing manifests.
- Run after upload completion when a piece becomes `available`.
- Run periodically, for example every 5 minutes.
- Keep each pass bounded so eviction cannot monopolize the session thread.

The timer should do selection on the session thread, but expensive disk work
must not block peer IO for long periods. If hole punching is fast enough in
practice it may happen inline for the first pass; otherwise use a small worker.

## Size Accounting

The initial target is simple and conservative:

- Count local pieces that also have manifest state `available`.
- If `usenet_cache_size_mib` is greater than zero and the counted bytes exceed
  the limit, evict oldest eligible pieces first.
- If age-based eviction is enabled and the minimum age is reached, evict
  eligible pieces even without size pressure.

This count is an approximation of reclaimable Usenet-backed local data, not a
replacement for Transmission's normal disk usage accounting.

## Failure Handling

If eviction fails:

- Leave local completion state unchanged.
- Leave manifest state unchanged.
- Log the error without credentials.
- Retry on a later pass.

If a later Usenet fetch fails for an evicted piece:

- Do not serve corrupt or missing data.
- Mark the manifest entry `failed` when hash, decode, or article lookup fails.
- The piece becomes unavailable unless local data exists again from another
  source.

## Tests

Focused unit tests:

- settings defaults and serialization for new keys
- manifest timestamp load/save compatibility
- candidate selection skips non-available pieces
- candidate selection skips pieces below minimum age
- candidate selection skips missing local pieces

Integration or manual tests:

- Complete/upload a small torrent, wait or force eligibility, verify local
  pieces are evicted.
- Confirm local completion drops while peer-facing availability remains.
- Request an evicted piece through a peer path and verify Usenet restore.
- Confirm total Usenet IO still respects the shared IO limit during restore.
- Confirm disabled eviction makes no local data changes.

## Acceptance Criteria

- With eviction disabled, behavior matches the current main branch.
- With eviction enabled, only manifest `available` pieces are evicted.
- Local completion decreases after successful eviction.
- Peer-facing availability still includes evicted Usenet-backed pieces.
- A peer request can restore an evicted piece from Usenet and pass hash check.
- No credentials are logged or written outside `.env` and temporary nyuu config
  files.
- Build and full test suite pass.

## Rollout Strategy

1. Add settings and documentation.
2. Extend manifest metadata with compatible timestamp fields.
3. Implement candidate selection without deletion.
4. Add safe local piece deallocation and local completion updates.
5. Wire the session timer and upload-completion trigger.
6. Add tests.
7. Run real Usenet restore/eviction validation with a small legal torrent.
8. Merge before starting Web UI work.
