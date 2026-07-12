# Usenet Multipart Piece Implementation Plan

Status: Active.

Design authority: [Usenet Multipart Piece Design](Usenet-Multipart-Piece-Design.md)

## Objective

Implement deterministic multi-article storage for BitTorrent pieces larger than
the configured Usenet article payload limit. The work is complete only when a
multipart piece can be uploaded, discovered, restored after local eviction, and
served to a BitTorrent peer without exceeding the shared Usenet IO limit.

This plan deliberately lands multipart reading before multipart publishing.
Until the uploader phase is complete, oversized local pieces must remain local
and must not be posted through the old single-article path.

## Baseline

Before implementation begins:

- branch from `main` after commit `05139fef0`;
- retain the 2 MiB default article payload limit;
- retain single-article Message-IDs and manifests;
- keep `tmp/`, `.env`, generated packages, runtime data, and provider
  credentials outside version control;
- record the current focused test baseline:

```sh
cmake --build build -j2 --target transmission-daemon libtransmission-test
build/tests/libtransmission/libtransmission-test \
  --gtest_filter='SessionTest.*:SettingsTest.*:UsenetPieceStoreTest.*:RpcTest.*'
```

No stage may weaken normal BitTorrent fallback or local piece checksum
verification.

## Implementation Progress

Completed on `feature/usenet-multipart-pieces`:

- deterministic base and continuation Message-IDs and bounded part planning;
- manifest v2 chain metadata with v1 compatibility;
- exact-size multipart download assembly and final piece hash verification;
- multipart Nyuu upload batches with complete-chain readback on ambiguous
  errors;
- discovery validation of complete sampled chains;
- related build, format, and focused regression checks (`95/95`).

Real-provider validation on 2026-07-12 used generated trackerless data with one
4 MiB piece and a 2 MiB article payload limit. Upload produced two articles and
persisted `article_count: 2`. A fresh Nashawk instance discovered the chain, a
normal BitTorrent client requested it, Nashawk restored the piece from Usenet,
and all three SHA-1 values (source, restored cache, and receiving client)
matched.

The first smoke attempt exposed a Nyuu template pitfall: `{fnamebase}` removes
multiple suffixes, mapping both `hash.piece` and `hash.1.piece` to `hash`. The
implementation now stages extensionless `hash`/`hash.1` files and uses
`{filename}`, preserving deterministic continuation IDs.

## Commit 1: Multipart Format Helpers

### Production changes

Add pure helpers, preferably in `usenet-piece-store.{h,cc}` unless extraction to
a small `usenet-piece-chain.{h,cc}` module materially improves ownership:

- derive the base Message-ID from a piece hash;
- derive continuation Message-IDs using `.1`, `.2`, and subsequent suffixes;
- reject malformed base IDs and continuation indices beyond the 1024-article
  safety bound;
- calculate upload part byte ranges from exact piece size and configured raw
  payload limit;
- preserve an unnumbered first part for both single and multipart pieces.

Change torrent admission so a piece larger than one article is not rejected
solely for that reason. Keep an explicit upload guard in
`onUsenetPieceCompleted()` that skips multipart publication until Commit 4 is
complete. The guard must leave the piece local and non-evictable.

Also defer the legacy base-only discovery path for torrents whose standard
piece size exceeds one article. Remove that guard only when Commit 5 can
validate complete sampled chains; otherwise an existing base article could
create a false availability claim during intermediate development.

### Tests

Extend `usenet-piece-store-test.cc` or add `usenet-piece-chain-test.cc` with:

- exact 2 MiB piece -> one unnumbered article;
- exact 4 MiB piece -> base plus `.1`;
- 4 MiB plus one byte -> base, `.1`, and one-byte `.2`;
- final torrent piece smaller than the standard piece size;
- zero payload limit rejected;
- deterministic IDs independent of torrent infohash;
- duplicate piece hashes produce identical chains;
- sequence boundary at 1024 articles;
- oversized torrents remain stopped only for real initialization errors, not
  because their piece size exceeds one article.

### Gate

- All pure helper and existing manifest tests pass.
- Adding a 4 MiB-piece torrent in Usenet mode does not stop or crash it.
- Completing such a piece does not invoke Nyuu yet and cannot trigger eviction.
- Legacy discovery remains `not_checked` for such a torrent.

## Commit 2: Manifest Version 2

### Production changes

Update `usenet-piece-store.{h,cc}`:

- write manifest version 2;
- add optional `article_count` and `article_payload_size` fields per logical
  piece;
- keep `message_id` as the base Message-ID;
- treat absent chain metadata as unknown rather than invalid;
- read version 1 manifests without rewriting them merely because they were
  loaded;
- upgrade to version 2 on the next meaningful state write;
- update all entries sharing a base Message-ID consistently;
- keep piece state atomic: there is no persisted per-article availability.

Add store methods that update logical piece state together with observed chain
metadata. Avoid a sequence of separate writes that can persist `available`
without its validated article count.

### Tests

- Existing version 1 fixtures load unchanged.
- Version 2 round-trips known and unknown article counts.
- Negative and over-1024 counts are rejected; zero is normalized to unknown
  chain metadata even when the logical piece state is available.
- Duplicate Message-ID entries receive the same state and chain metadata.
- Interrupted uploads reset to `unknown` without retaining misleading
  availability.
- Existing timestamp and discovery fields survive migration.

### Gate

- All `UsenetPieceStoreTest` tests pass.
- A copied version 1 runtime manifest can start and stop the daemon without
  modification or loss of state.

## Commit 3: Multipart Download And Assembly

### Service layer

Extend `usenet-service.{h,cc}` with a chain download operation:

- open, authenticate, and select the group once;
- issue sequential `BODY` requests for the base and continuation IDs;
- decode each article independently without requiring it to equal the complete
  piece size;
- reject empty parts, accumulated overflow, missing required continuations,
  malformed yEnc, and more than 1024 articles;
- stop only at the exact metainfo piece size;
- return complete bytes and observed article count;
- keep the existing single-article API as a wrapper or migrate all callers in
  the same commit.

Extract the chain-control loop behind an injectable BODY-fetch callback so its
sequencing and failure behavior can be tested without a real provider. Network
code remains responsible only for connection and NNTP response handling.

### Session integration

Update `session.{h,cc}`:

- download tasks carry expected piece size and hash context;
- one complete chain operation consumes one shared IO slot;
- the worker assembles off the session thread;
- `onUsenetPieceDownloaded()` writes only exact-size complete data;
- `install_recovered_piece()` remains the final hash and storage guard;
- successful reads persist observed chain metadata;
- failed reads mark the piece non-serviceable before returning to BitTorrent
  fallback;
- partial buffers never reach `tr_ioWrite()`.

### Tests

Add a dedicated service/chain test target if needed and cover:

- old single article success;
- two and three article success;
- reader limit differs from uploader part size;
- missing base and missing continuation;
- empty middle part;
- malformed yEnc part;
- decoded bytes exceed expected piece size;
- exact size with wrong piece hash;
- 1024-part bound;
- cancellation and cleanup while the session is closing;
- failure clears Usenet serviceability and permits BT fallback.

### Gate

- Unit tests prove the exact requested Message-ID sequence.
- Existing single-article real data still restores.
- A manually prepared multipart chain restores through the daemon before
  multipart upload is enabled.
- Shared runtime IO counters never exceed the configured limit.

## Commit 4: Multipart Upload And Nyuu Batching

### Staging and task model

Update `session.{h,cc}`:

- replace one temporary file per upload task with a logical piece task owning a
  bounded vector of part files;
- split the existing complete staged piece into deterministic contiguous part
  files;
- assign filename bases `<hash>`, `<hash>.1`, and subsequent suffixes;
- flatten part files into Nyuu batches while retaining logical piece completion
  groups;
- count `MaxNyuuBatchFiles` in articles, not pieces;
- clean every full-piece and part file on all exits.

### Completion and retry

- Keep the logical piece `uploading` until all parts succeed.
- Never mark a piece `available` from partial batch success.
- On an ambiguous batch or duplicate-ID error, reconstruct the complete remote
  chain and compare it with the complete local piece.
- Treat a complete match as success even when the remote uploader used a
  different part size.
- Treat missing or mismatched chains as failure and retain local data.
- Retry interrupted uploads as complete logical pieces.
- Persist `article_count` and upload `article_payload_size` atomically with the
  available state.
- Remove the temporary oversized-upload guard introduced in Commit 1 only after
  these paths pass.

### Service changes

Keep Nyuu token expansion for batch Message-IDs. Confirm that staged filenames
produce the required local parts and that each staged file is smaller than or
equal to the configured raw payload limit. Nyuu's internal `article-size`
margin remains an implementation detail and must not alter deterministic part
boundaries.

### Tests

- 1, 2, and 3-part staging with exact byte reconstruction.
- Mixed batch containing single and multipart pieces.
- Partial batch failure never marks a piece available.
- Duplicate-ID error with equal complete remote piece succeeds.
- Duplicate-ID error with incompatible mixed chain fails.
- Interrupted upload requeues one logical piece without leaking files.
- Shutdown cleans queued and active multipart staging.
- Eviction remains blocked until complete upload confirmation.
- IO slot accounting includes Nyuu upload and post-check connections.

### Gate

- Generated 4 MiB data uploads as exactly two articles at the 2 MiB default.
- Both articles read back and reconstruct byte-for-byte.
- A second node configured with a different payload limit restores the chain.
- No temporary files remain after success, failure, or shutdown.

## Commit 5: Multipart Discovery

### Production changes

Update discovery structures and workers in `session.{h,cc}`:

- samples carry piece index, base Message-ID, expected size, and piece hash;
- retain base `STAT` as a cheap missing check;
- validate every positive sample by downloading its complete chain;
- classify missing base/continuation as `missing`;
- classify transport, decoding, overflow, and hash failures as `error`;
- never mark all pieces available from base-article existence alone;
- persist observed article counts for sampled pieces where useful.

Add a discovery decoded-byte budget with a default of 32 MiB:

- add a session setting and quark;
- expose a daemon CLI override in MiB or bytes with unambiguous naming;
- include it in settings serialization and RPC session information;
- keep sample count as an independent upper bound;
- complete at least one selected sample, then stop starting new samples after
  the byte budget is reached.

Discovery remains sequential and each sampled chain uses one shared IO slot.

### Tests

- Base exists but `.1` missing -> discovery `missing`.
- Complete chain with correct hash -> `available`.
- Exact bytes with wrong hash -> `error`.
- Single-article legacy sample still succeeds.
- Byte budget reduces effective samples deterministically.
- One sample may complete when larger than the budget.
- Discovery never exceeds the shared IO limit.
- Runtime and RPC counters report requested and effective sample work.

### Gate

- A fresh node discovers a remotely uploaded 4 MiB-piece torrent.
- Deleting local data and requesting a sampled or unsampled piece restores it.
- Incomplete remote chains do not produce `Ready`, `Mixed ready`, or advertised
  availability.

## Commit 6: Integration, UI Diagnostics, And Documentation

### Integration matrix

Run deterministic generated-data tests, followed by a small real torrent whose
piece size exceeds 2 MiB:

| Scenario | Expected result |
| --- | --- |
| 2 MiB piece | Existing single article behavior |
| 4 MiB piece | Base plus `.1`, complete restore |
| 4 MiB + remainder | Base plus numbered continuation and short final part |
| Different node limits | Reader reconstructs uploader-defined chain |
| Missing continuation | Usenet failure, BT fallback, no false ready state |
| Corrupt continuation | Hash failure, BT fallback, no local commit |
| Local eviction | Allowed only after complete-chain availability |
| Peer request after eviction | Restore from Usenet, then serve valid block |
| Concurrent upload/download | Active IO never exceeds configured limit |
| Restart during upload | Safe reset and complete logical-piece retry |

### Diagnostics

Extend trace/debug logs and RPC details with:

- logical piece index;
- article count and current continuation index;
- accumulated and expected decoded bytes;
- discovery effective sample count and byte usage;
- complete-chain retry/readback outcome.

Never log credentials, article bodies, or private tracker URLs. Normal info-level
output remains concise.

The Web UI continues to classify logical piece availability. Add article counts
only where they clarify debug state; do not turn the normal torrent list into an
article-level interface.

### Documentation

- Update `Usenet-Piece-Backend-README.md` with multipart behavior and limits.
- Update script documentation for the discovery byte budget.
- Mark the design and this plan implemented only after all gates pass.
- Move both documents into `docs/archive/` during the final merge preparation.

## Required Test Commands

Focused tests after each commit:

```sh
cmake --build build -j2 --target transmission-daemon libtransmission-test
build/tests/libtransmission/libtransmission-test \
  --gtest_filter='SessionTest.*:SettingsTest.*:UsenetPieceStoreTest.*:UsenetPieceChainTest.*:RpcTest.*'
```

Before merge:

```sh
ctest --test-dir build --output-on-failure
bash -n scripts/run-usenet-webui-test.sh scripts/package-vps-deploy.sh
git diff --check main...HEAD
```

Run the real provider smoke tests at low volume. Do not use real provider tests
as a substitute for deterministic unit and integration coverage.

## Stop Conditions

Pause implementation and investigate before continuing if any stage causes:

- a normal torrent to stop because multipart Usenet data is absent;
- local eviction before complete-chain confirmation;
- partial reconstructed bytes to reach torrent storage;
- discovery to mark ready from only the base article;
- IO counters to exceed the configured shared limit;
- different reader/upload limits to change Message-ID lookup behavior;
- repeated unbounded BODY requests;
- leaked staging files or worker shutdown hangs;
- credentials or private tracker URLs in logs or committed fixtures.

## Merge Criteria

The feature branch is mergeable only when:

- all six commits or equivalent independently reviewable changes are complete;
- every stage gate and the integration matrix pass;
- existing single-article content remains readable;
- multipart upload is not enabled without matching download and discovery
  support;
- the real 2 MiB provider check and a 4 MiB multipart roundtrip pass;
- local deletion followed by Usenet restoration and BitTorrent serving is
  demonstrated;
- documents describe actual behavior and completed plans are archived;
- the worktree contains no credentials, private torrent fixtures, runtime data,
  or generated deployment packages.
