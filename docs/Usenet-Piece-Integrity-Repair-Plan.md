# Usenet Piece Integrity And Repair Plan

Status: In progress on `fix/usenet-piece-integrity-repair`.

## Objective

Prevent local piece eviction unless the exact remote Usenet article chain has
been read back, decoded, assembled to the metainfo size, and matched against the
BitTorrent piece SHA-1. Add a torrent-level integrity audit that can run
automatically before a torrent becomes Usenet Ready and manually from the Web
UI. Missing or corrupt remote data must be repaired from a verified local piece
or reacquired from BitTorrent before it can become evictable again.

## Confirmed Failure Mode

Sampled discovery currently validates a small deterministic subset and then
marks every manifest entry `available`. Local eviction treats that state as a
per-piece verification result. An unsampled piece can therefore be removed even
though its article chain was never read from Usenet.

The current upload success path also accepts Nyuu's successful exit as remote
availability without requiring an independent complete readback. Readback is
only mandatory after an ambiguous upload error. A successful NNTP POST response
is useful evidence, but it is not strong enough to authorize deletion of the
only local copy.

## Safety Invariants

1. Sampled discovery may establish optimistic serviceability, but never local
   eviction eligibility.
2. Only a complete `BODY` chain, successful yEnc decode, exact piece size, and
   matching piece SHA-1 establish a per-piece verification credential.
3. A successful upload subprocess exit does not establish that credential.
4. Manifests written by older versions have no trusted verification credential,
   even when their state is `available`.
5. Integrity failure immediately withdraws the affected piece from peer-facing
   Usenet availability before repair starts.
6. Repair may only upload bytes from a locally hash-verified piece. If no local
   piece exists, normal BitTorrent acquisition is used and upload is queued only
   after Transmission verifies the completed piece.
7. A torrent is Usenet Ready only when every metainfo piece has a current
   per-piece verification credential. Local presence alone does not complete a
   Usenet integrity audit.
8. All audit, readback, and repair work obeys the existing shared Usenet IO
   limit and runs outside the session thread.

## Persistent Model

Upgrade the manifest format and add these fields to each piece entry:

- `verified_at`: time of the most recent complete remote readback and hash
  validation; zero means the piece is not eligible for eviction.
- `verification_source`: `upload_readback`, `audit`, or `on_demand_read` for
  diagnostics. This field does not change the strength of the credential.
- `last_error`: concise non-secret failure information for repair and UI
  diagnostics.

Add a torrent-level integrity section:

- state: `not_checked`, `checking`, `repairing`, `ready`, `incomplete`, or
  `error`;
- checked, verified, missing, corrupt, repairing, and waiting-for-peers counts;
- start and finish timestamps;
- a generation identifier so stale worker results cannot overwrite a newer
  manual audit.

Existing version 1 and 2 manifests remain readable. Their `available` entries
retain optimistic serviceability for compatibility, but load with
`verified_at = 0` and cannot trigger eviction until revalidated.

## Discovery Semantics

Discovery remains deliberately sampled to avoid abusive provider query rates.
A complete sampled chain can mark a torrent as optimistically discovered and
permit Usenet-first retrieval. It must not synthesize per-piece verification
timestamps.

Unsampled pieces may remain peer-visible after a successful discovery, but an
actual request performs the existing complete chain and hash validation. A
failure withdraws that piece and falls back to BitTorrent. This optimistic state
must be represented separately from a verified per-piece state so eviction
cannot confuse the two.

## Full Integrity Audit

The audit walks every piece with bounded concurrency:

1. Fetch the base and required continuation articles with `BODY`.
2. Decode each yEnc body and assemble exactly the metainfo piece size.
3. Verify the final BitTorrent piece SHA-1.
4. Persist the observed article count, payload size, verification timestamp,
   and source.
5. Discard fetched bytes unless the local piece is absent and installation is
   explicitly needed by an active peer request.

The Web UI exposes `Verify Usenet Data` for one or more selected torrents. RPC
starts the work asynchronously and the existing torrent refresh stream reports
progress and the final result.

An automatic audit starts when a torrent first reaches the candidate condition
where all pieces are locally present or optimistically Usenet-serviceable. The
UI shows `Verifying Usenet`, not `Usenet Ready`, until the audit succeeds.

## Repair Flow

For every missing, truncated, malformed, or hash-mismatching chain:

- mark the remote piece unavailable immediately;
- if a verified local piece exists, queue a replacement upload followed by a
  mandatory independent readback;
- otherwise leave it unavailable and allow BitTorrent to request it;
- after BitTorrent completes and verifies the piece, queue upload and mandatory
  readback automatically;
- keep the torrent in `Repairing` while work is active and `Incomplete` while
  any piece is waiting for peers;
- return to `Ready` only after all failed pieces pass remote readback.

If neither Usenet nor BitTorrent can supply a piece and no local copy exists,
the loss is not recoverable by Nashawk. The UI and logs must state this instead
of retaining a Ready label.

## Additional Risk Reduction

- Require mandatory independent readback after every successful Nyuu upload.
- Add a configurable stabilization delay before upload readback so the check is
  not performed only against an immediate posting path.
- Recheck the remote chain immediately before eviction when the last full
  verification is older than a conservative freshness window.
- Add low-frequency, staggered periodic audits for Ready torrents, disabled or
  conservatively scheduled by default until real-provider load is measured.
- Apply bounded retries with exponential backoff and distinguish transient NNTP
  errors from definitive missing/corrupt data.
- Record counters and non-secret errors in RPC and logs so failures can be
  diagnosed without credentials or article bodies.

## Delivery Order

### Phase 1: Stop Data Loss

- add persistent verification credentials with backward-compatible loading;
- make eviction require a nonzero trusted `verified_at`;
- prevent sampled discovery from creating trusted credentials;
- add regression tests proving old and discovery-derived availability cannot be
  evicted.

### Phase 2: Mandatory Upload Readback

- route every successful Nyuu result through complete-chain readback;
- mark `available` and set `verified_at` only after SHA-1 validation;
- retain local data and expose a failure when readback does not succeed;
- test successful, missing continuation, corrupt, transient, and restart cases.

### Phase 3: Audit And Repair Engine

- implement cancellable torrent-level jobs and progress state;
- verify all pieces with bounded shared IO;
- reupload from local data or transition to BitTorrent reacquisition;
- trigger one audit automatically before the first Ready transition.

### Phase 4: RPC And Web UI

- add a torrent RPC action for `Verify Usenet Data`;
- expose integrity state, counts, timestamps, and last error;
- add the right-click action with correct disabled/running states;
- distinguish `Verifying`, `Repairing`, `Waiting for peers`, and `Ready` in the
  torrent list and inspector.

### Phase 5: Validation

- focused manifest, eviction, worker, RPC, and UI tests;
- full project test suite and formatting checks;
- real-provider test with intentional article removal/substitution where the
  provider permits it, otherwise use a controlled missing Message-ID fixture;
- prove local data is retained before verification, local repair reuploads, and
  no-local repair returns to BitTorrent before becoming Ready;
- update the backend README and archive this plan only after all gates pass.

## Merge Gate

The branch is not mergeable until:

- no sampled or legacy state can authorize eviction;
- every new eviction candidate has passed complete remote hash verification;
- automatic and manual audits share one bounded implementation;
- missing remote pieces are withdrawn and follow the documented repair path;
- the Web UI reports actionable progress and outcome;
- focused, full-suite, and real-provider tests pass without leaking credentials
  or temporary piece data.
