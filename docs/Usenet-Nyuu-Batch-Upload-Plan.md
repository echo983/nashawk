# Usenet Nyuu Batch Upload Plan

Status: active design/implementation plan.

## Problem

The first Usenet uploader posts one completed BitTorrent piece per `nyuu`
process. Raising `usenet_upload_concurrency` therefore creates many independent
Node/Nyuu processes. In real daemon tests, high process concurrency was unstable
with `nyuu` 0.4.x, while low process concurrency was reliable but did not use a
provider account's available NNTP connections efficiently.

## Goal

Use Nyuu's own multi-connection uploader efficiently without changing Nashawk's
storage model:

- one BitTorrent piece still maps to exactly one Usenet article;
- the Message-ID local part is still the piece hash;
- no NZB dependency is introduced;
- downloads continue to fetch by deterministic Message-ID;
- the shared Usenet IO limit continues to cap upload and download activity
  together.

## Design

Upload workers should drain several queued piece tasks into one Nyuu batch. The
batch uploader stages deterministic per-piece filenames, then invokes one Nyuu
process with multiple input files.

Nyuu token expansion will be used for per-file identity:

- staged filename: `<piece-hash>.piece`
- `message-id`: `{fnamebase}@nashawk.local`
- `subject`: `Nashawk piece {fnamebase}@nashawk.local`
- `yenc-name`: `{filename}`

Nyuu receives an `article-size` larger than the raw torrent piece size, leaving
room for yEnc expansion and article overhead. This keeps each staged piece in a
single article. If a piece cannot fit into one provider-supported article even
with this margin, the torrent must be rejected by the one-article policy rather
than split.

The batch chooses its Nyuu connection count from the number of files in the
batch and the shared Usenet IO limit, then caps a single Nyuu process at a
conservative local maximum. Nyuu's post-check connection is also reserved from
the shared IO budget. Before spawning Nyuu, the worker acquires the upload
connections plus the check connection; after Nyuu exits it releases the same
count. Downloads still acquire one slot each, so upload and download cannot
exceed the configured server connection budget together.

## Failure Handling

Nyuu has already shown cases where it reports an error after the article is
actually available. Batch upload therefore keeps per-piece verification:

1. If Nyuu exits successfully, all pieces in the batch become available.
2. If Nyuu exits with an error, each piece is retried through the conservative
   single-file Nyuu upload path.
3. Pieces whose single-file retry succeeds become available.
4. Pieces whose single-file retry also fails are fetched back by Message-ID and
   compared with the staged local bytes.
5. Pieces that pass readback become available.
6. Pieces that fail readback are marked failed and retain the error details.

This makes batch upload conservative: it may spend extra readback IO on error
paths, but it should not silently claim missing pieces.

## First Implementation Scope

- Add a batch upload request to `usenet-service`.
- Generate a tokenized Nyuu config for batch uploads.
- Let upload workers drain bounded batches from the existing queue.
- Stage batch filenames in a private temporary directory under the existing
  Usenet temp directory.
- Extend the Usenet IO limiter to acquire and release multiple slots.
- Keep the existing single-file uploader available for checks and fallback.
- Update runtime logs and documentation to describe effective process and
  connection behavior.

## Validation

- Build the project.
- Run focused Usenet service/unit tests where available.
- Run a low-volume real upload smoke test with a small batch.
- Confirm runtime stats never report active Usenet IO above the configured
  limit.
- Confirm uploaded pieces can be deleted locally and fetched back from Usenet.
