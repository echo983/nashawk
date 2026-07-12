# Usenet Multipart Piece Design

Status: Proposed.

Implementation plan: [Usenet Multipart Piece Implementation Plan](Usenet-Multipart-Piece-Implementation-Plan.md)

## Goal

Allow every supported BitTorrent piece to use the Usenet backend even when the
piece is larger than the provider-safe single-article payload limit. A piece may
span a deterministic chain of articles, while the BitTorrent metainfo remains
the authority for its expected byte length and SHA-1 hash.

The change must preserve the existing properties of the backend:

- no NZB or external index dependency;
- deterministic lookup from a BitTorrent piece hash;
- no search across arbitrary Usenet content;
- normal BitTorrent fallback when Usenet data is absent or invalid;
- local eviction only after the complete piece is recoverable;
- one shared IO limit across upload, download, and discovery.

## Terminology

- **Piece**: one BitTorrent piece described by torrent metainfo.
- **Part**: one contiguous byte range of a piece staged for upload.
- **Article**: one Usenet article containing one independently yEnc-encoded
  part.
- **Base Message-ID**: the existing `<piece-hash>@nashawk.local` identity.
- **Continuation Message-ID**: a deterministic numbered Message-ID for every
  part after the first.

## Wire Format

The first article keeps the current Message-ID unchanged. Continuations append
a decimal sequence number to the local part:

```text
<piece-hash>@nashawk.local
<piece-hash>.1@nashawk.local
<piece-hash>.2@nashawk.local
...
```

The unnumbered article is implicit part zero. `.1` is the second part. Sequence
numbers have no leading zeroes.

Each article is an independent yEnc document whose decoded bytes are the next
contiguous range of the piece. Correctness does not depend on Subject,
Newsgroups, yEnc filename, custom headers, or the uploader's configured part
size. The Message-ID chain, torrent piece size, and final piece hash are the
protocol contract.

This format provides the following compatibility behavior:

- Existing single-article pieces remain unchanged and are read as one-part
  chains.
- New readers can consume articles written by existing Nashawk versions.
- Old readers will reject the first article of a multipart piece because its
  decoded size or final piece hash does not match. They do not accept corrupt
  data, but they cannot restore multipart pieces.
- Equal piece hashes share the same article chain, including duplicate pieces
  within one torrent or across torrents.

## Configuration Semantics

`usenet_check_article_size` becomes the maximum raw payload bytes in one
Nashawk article part. Its conservative default remains 2 MiB. Startup validation
must continue to prove that one part of this size can be posted and read back by
the configured provider account and posting path.

The value is an upload policy, not a property needed by readers. A reader must
not calculate the remote article count from its own configured limit because a
different node may have uploaded the chain with a different part size.

Changing the configured limit affects future uploads only. Existing chains stay
readable because download termination is based on accumulated decoded bytes.

## Upload Algorithm

For a completed local piece:

1. Read the complete piece into the existing private staging area.
2. Split it into contiguous files of at most `usenet_check_article_size` raw
   bytes. The final file contains the remainder.
3. Name staged files so Nyuu's filename tokens produce the deterministic base
   and continuation Message-IDs.
4. Upload all parts through the existing bounded Nyuu batch machinery.
5. Keep the manifest piece state `uploading` until every article succeeds or
   the complete remote chain passes readback verification.
6. Mark the piece `available` only after the whole chain is known to be
   recoverable.
7. Remove all temporary part files on success, failure, shutdown, or retry.

For a piece hash `abc` split into three parts, staged filename bases are `abc`,
`abc.1`, and `abc.2`. The current batch template then produces the required
Message-IDs without an external index.

Nyuu batches count article part files rather than logical pieces. Completion is
still reported per piece. A batch may contain parts from several pieces, but a
piece cannot become available after only a subset of its parts succeeds.

### Upload Failure And Duplicate Articles

Usenet articles are effectively immutable and another node may already have
posted the same piece using a different part size. In that case the first
article's bytes can differ even though the complete reconstructed piece is
identical.

Therefore, after an ambiguous Nyuu error or duplicate Message-ID response,
Nashawk must read and reconstruct the complete remote chain and compare it with
the complete staged piece. Per-part equality is not sufficient. If the complete
piece matches, the upload succeeds. If any article is missing or the final bytes
do not match, the piece is marked failed and remains local.

Interrupted multipart uploads are retried as complete logical pieces. Reposting
already accepted parts is safe because their Message-IDs are deterministic;
final success still requires the complete-chain check.

## Download Algorithm

A download task carries the base Message-ID, expected piece size, and expected
piece hash. It holds one shared Usenet IO slot and should reuse one authenticated
NNTP connection for the article chain.

1. Fetch and independently yEnc-decode the base article.
2. Reject an empty decoded part.
3. Append the decoded bytes to a private buffer or temporary file.
4. If accumulated bytes are smaller than the expected piece size, fetch `.1`,
   then `.2`, and so on.
5. Reject a part if appending it would exceed the expected piece size.
6. Stop only when accumulated bytes equal the expected piece size.
7. Verify the complete BitTorrent piece hash.
8. Commit the piece to local torrent storage only after all checks pass.

The implementation must never expose a partially reconstructed piece through
the BitTorrent storage path. The existing `install_recovered_piece()` checksum
remains the final integration guard even if the worker also verifies the hash
before returning.

Failure of the base article, any required continuation, yEnc decoding, size
validation, or final hash validation fails the Usenet attempt for that piece.
The partial buffer is discarded, the piece is no longer advertised as
Usenet-serviceable, and normal BitTorrent download remains available.

The reader must enforce a defensive maximum of 1024 articles per piece. Reaching
that limit before the expected byte count is an invalid chain, not a reason to
continue querying. This is a malformed-data safety bound; it does not reject the
torrent from normal BitTorrent operation.

## Discovery

The current discovery implementation uses `STAT` on only the base Message-ID.
That is insufficient for multipart pieces because it can declare a torrent
ready when only the first part exists.

Multipart-aware discovery validates each sampled piece as follows:

1. Use `STAT` as a cheap negative check for the base Message-ID.
2. If it exists, fetch and reconstruct the complete sampled piece chain.
3. Validate exact piece size and the BitTorrent piece hash.
4. Treat a missing continuation as `missing`, malformed content as `error`, and
   only a valid complete chain as `available`.

This costs more read traffic than base-only `STAT`, so discovery remains
deterministic, sequential, and bounded. Add a decoded-byte budget with a proposed
default of 32 MiB per discovery run. At least one sampled piece may complete even
when it exceeds the budget; no additional sample starts after the budget is
reached. The existing sample-count setting remains a second upper bound.

Only a complete pass over the effective sample set may optimistically mark the
torrent Usenet-backed. Actual piece requests still validate their own complete
chains and fall back independently.

## Manifest Version 2

Piece availability remains a logical piece state. Per-part state is deliberately
not persisted because incomplete chains are not serviceable and interrupted
uploads are retried as complete pieces.

Version 2 adds optional chain metadata to each piece entry:

```json
{
  "status": "available",
  "message_id": "<piece-hash>@nashawk.local",
  "article_count": 3,
  "article_payload_size": 2097152
}
```

- `message_id` remains the base Message-ID.
- `article_count` is zero or absent when unknown, otherwise the validated count.
- `article_payload_size` records the uploader's chosen non-final part size for
  diagnostics and retry optimization. Readers do not depend on it.

The reader accepts version 1 manifests. Existing version 1 available entries
are treated as one-part chains until an actual read proves otherwise. A
successful dynamic chain read updates the entry with the observed article count.
Writing a changed manifest upgrades it to version 2 without changing existing
Message-IDs or piece states.

RPC summaries continue to count logical pieces, not articles. Detailed debug
output may include article count, bytes accumulated, continuation Message-ID,
and the failing part number, but credentials and article bodies must never be
logged.

## State And Eviction Rules

- `unknown`: no complete chain is known.
- `uploading`: one or more articles may exist, but the complete chain is not yet
  confirmed.
- `available`: the complete chain was uploaded, read back, or discovered and
  validated according to the applicable policy.
- `failed`: the latest complete-chain upload or read validation failed.

Automatic local eviction requires logical piece state `available`; individual
article success never makes a piece eviction-eligible. A failed restore clears
serviceability for that piece before BitTorrent fallback.

## Resource And Abuse Bounds

- One chain operation consumes one shared Usenet IO slot even though it may issue
  several NNTP commands on one connection.
- Upload batches remain bounded by staged article file count and the existing
  Nyuu connection budget.
- No empty part is accepted.
- Accumulated bytes may never exceed the metainfo piece size.
- No more than 1024 BODY requests are issued for one piece chain.
- Discovery obeys both sample-count and decoded-byte limits.
- Temporary storage is bounded to approximately one logical piece per active
  upload or download task and is always cleaned up.

## Non-Goals For The First Version

- Adaptive retry with progressively smaller part sizes after a provider rejects
  the configured size.
- Repairing only selected missing continuation articles.
- Publishing NZB files or a semantic content index.
- Searching by Subject, filename, group contents, or anything other than the
  deterministic Message-ID chain.
- Parallel BODY retrieval inside one piece. Piece chains are sequential; normal
  concurrency comes from independent piece tasks under the shared IO limit.

## Implementation Phases

### Phase 1: Format And Pure Helpers

- Add deterministic continuation Message-ID helpers.
- Add part-range planning with exact first/final-piece behavior.
- Add manifest v2 serialization and v1 migration tests.
- Remove the current rule that rejects a torrent solely because its piece size
  exceeds one article payload.

### Phase 2: Multipart Download

- Add a chain download API that reuses one NNTP connection.
- Decode parts without requiring each article to equal the full piece size.
- Assemble with strict size and 1024-part bounds.
- Verify and atomically install only the complete piece.
- Add single-article compatibility, missing continuation, oversized chain,
  empty part, and hash-failure tests.

Download support lands before multipart upload so a new node can safely consume
and verify the format before it starts publishing it.

### Phase 3: Multipart Upload

- Split staged pieces into deterministic part files.
- Flatten part files into bounded Nyuu batches while retaining per-piece
  completion groups.
- Replace per-article error readback with complete-chain verification.
- Preserve cleanup, interrupted-upload recovery, IO accounting, and idempotency.

### Phase 4: Multipart Discovery

- Replace base-only positive `STAT` sampling with complete-chain validation.
- Add the decoded-byte budget and runtime counters.
- Confirm optimistic torrent readiness is set only after all effective samples
  validate.

### Phase 5: Integration And Operations

- Exercise single-article and multipart pieces against the real provider.
- Test nodes with different configured article payload limits.
- Test local eviction followed by multipart restoration and peer upload.
- Extend Web/RPC debug information without changing normal Transmission-facing
  behavior.
- Update the operator README and archive this design after merge.

## Acceptance Criteria

- A 4 MiB piece uploads as two 2 MiB articles with the specified Message-IDs.
- A non-multiple piece size produces a smaller final article and reconstructs
  exactly.
- Existing single-article content remains readable.
- A reader configured for a different article limit reconstructs the same chain.
- Missing or corrupt continuations never produce a local piece or a false
  availability claim.
- Duplicate upload errors succeed only when complete remote reconstruction
  matches the local piece.
- Discovery never marks a multipart sample available from the base article
  alone.
- Local eviction occurs only after all articles for the piece are confirmed.
- Upload, download, and discovery remain within the shared IO limit.
- Normal BitTorrent fallback continues after every Usenet failure mode.
