# Usenet Evidence-Driven Discovery Design

Status: Implemented on `feature/usenet-evidence-driven-discovery`; real-provider
validation pending.

## Objective

Stop automatically probing Usenet when a torrent merely obtains metainfo.
Automatic discovery must have evidence from normal piece upload traffic that a
compatible Nashawk dataset probably already exists. The only automatic trigger
is a threshold of distinct piece hashes whose upload received NNTP `441
Message-ID is not unique` and whose existing remote article chain was then read
back and matched to the local BitTorrent piece.

Add an explicit `Discover on Usenet` Web UI action and RPC method for operators
who want to probe a cold torrent before BitTorrent has supplied enough pieces to
produce duplicate-upload evidence.

## Non-Goals

- Do not infer discovery from a raw `441` count.
- Do not automatically query Usenet when metainfo first arrives.
- Do not change deterministic piece Message-IDs or multipart numbering.
- Do not let discovery authorize local eviction.
- Do not introduce an index, catalog, NZB dependency, or torrent-level Usenet
  identifier.
- Do not guarantee automatic discovery for a torrent that has no local pieces
  and cannot obtain any from BitTorrent.

## Evidence Definition

A piece contributes one unit of automatic discovery evidence only when all of
the following are true:

1. Nashawk attempted to upload that local, BitTorrent-verified piece as part of
   normal operation.
2. Nyuu reported NNTP `441` with `Message-ID is not unique` for at least one
   article belonging to that piece.
3. Nashawk fetched the complete base and continuation article chain using the
   deterministic Message-ID.
4. yEnc decoding succeeded, the assembled byte count matched metainfo, and the
   bytes matched the staged local piece and its BitTorrent SHA-1.
5. This base piece Message-ID has not already contributed evidence for this
   torrent.

A multipart piece still contributes at most one unit. Two piece indices with
the same SHA-1 also contribute one unit because they address the same remote
object. A `441` followed by `BODY 430`, a truncated chain, decode failure, size
mismatch, or hash mismatch contributes no evidence and follows the existing
upload failure path.

Successful readback after an unclassified timeout or batch failure proves that
the piece is remotely usable, but does not prove a duplicate upload and must not
count toward this trigger.

## Trigger Threshold

The first implementation uses both an absolute and proportional threshold:

```text
required pieces = min(3, torrent piece count)
duplicate evidence pieces >= required pieces
duplicate evidence pieces / distinct completed upload attempts >= 50%
```

Only attempts made since the current evidence window began are included in the
ratio. Each distinct base piece Message-ID counts once in the numerator and
denominator. This prevents duplicate-content positions or a few shared pieces
in a large torrent from triggering discovery while allowing a small torrent to
qualify. A tiny torrent with repeated identical piece hashes may require manual
discovery because it cannot supply enough independent automatic evidence.

The evidence window closes when discovery starts. A failed automatic discovery
starts a new window only after a conservative cooldown. Thresholds should be
constants in the first implementation; configuration is justified only after
field data shows a real need.

## Trigger Policy

### Automatic

Remove the current call that queues discovery from `ensureUsenetTorrent()` when
metainfo becomes available. Also prevent the initial automatic integrity audit
from probing a fresh local-complete torrent whose remote state is still
unknown. An integrity audit may start automatically only after:

- evidence-driven or manual discovery succeeds; or
- normal uploads and mandatory readbacks have independently verified every
  piece.

When the duplicate evidence threshold is reached, queue one automatic discovery
job for the torrent. `Checking`, an already successful discovery, or an active
integrity audit suppresses another automatic trigger.

### Manual

Add RPC method:

```text
torrent_usenet_discover
```

Add a torrent context menu action:

```text
Discover on Usenet
```

The manual action does not require duplicate evidence. It is available when
Usenet and discovery are enabled and the torrent has metainfo. It may start from
`NotChecked`, `Missing`, or `Error`, and may deliberately recheck `Available`.
It is disabled while discovery or an integrity audit is already checking the
torrent.

RPC starts the job asynchronously and returns per-torrent validation errors in
the same style as `torrent_usenet_verify`.

## Nyuu Result Classification

The current synchronous subprocess API reports only success or failure while
Nyuu writes article errors to inherited stderr. Add a bounded subprocess-output
path for Nyuu uploads:

- capture stderr with a strict maximum size;
- preserve the process exit status separately from captured diagnostics;
- parse only exact `441` duplicate responses and normalized Message-IDs;
- map continuation IDs back to their base piece ID;
- discard captured text after classification;
- return structured duplicate Message-IDs with the upload result;
- continue logging concise counters and sanitized errors without credentials,
  article bodies, or generated Nyuu configuration.

Truncated capture must disable duplicate classification for unparsed output; it
must never promote uncertain evidence. Existing generic error recovery remains
available.

## Discovery Sampling

Keep deterministic first, middle, last, and infohash-seeded sampling. For an
evidence-triggered run, prefer pieces that have not already passed duplicate
readback so the discovery adds independent evidence. If too few unverified
pieces remain, fill the sample with verified evidence pieces.

Manual discovery uses the same selection rule. If the configured sample size is
greater than or equal to the piece count, every piece is checked.

Every sampled piece still requires complete-chain fetch, exact size, and SHA-1
validation. One failed sample makes discovery `Missing`; an interrupted worker
makes it `Error`.

## Upload Queue Transition

Reaching the threshold must prevent a large queue of redundant POST attempts:

1. Mark the torrent as discovery `Checking` on the session thread.
2. Stop accepting new ordinary upload tasks for that torrent.
3. Allow already executing Nyuu work to finish.
4. Remove not-yet-started tasks for that torrent from the shared upload queue,
   delete their staged temporary files, and reset their `Uploading` entries to
   `Unknown` unless another duplicate piece shares an already verified
   Message-ID.
5. Leave tasks belonging to other torrents untouched.
6. Queue discovery through the existing shared Usenet IO limiter.

This transition must be idempotent and restart-safe. It must not strand manifest
entries in `Uploading`.

## State Merge After Discovery

Discovery results must merge with upload evidence instead of replacing the
manifest wholesale.

On success:

- preserve `Available` pieces and their verification credentials;
- preserve explicit `Failed` pieces because a torrent-level sample is weaker
  than a piece-specific failure;
- convert remaining `Unknown` pieces to optimistic `Available` with
  `verified_at = 0`;
- reset cancelled `Uploading` pieces to optimistic `Available`;
- record sampled pieces as verified only after complete hash validation;
- start the bounded full integrity audit;
- upload only pieces that the audit proves missing and that exist locally.

On `Missing` or `Error`:

- retain independently verified remote pieces;
- do not synthesize availability for unknown pieces;
- clear the discovery upload hold;
- rescan verified local pieces and resume ordinary uploads;
- retain a concise error and cooldown timestamp.

Neither outcome may make an unverified piece eligible for eviction.

## Persistence And Observability

Extend the manifest discovery section with backward-compatible optional fields:

- trigger: `none`, `duplicate_evidence`, or `manual`;
- distinct attempted piece count for the active evidence window;
- duplicate-verified piece indices with unique base Message-IDs, bounded to
  what the threshold needs;
- evidence-window start time and automatic retry cooldown time.

Persisting the evidence set prevents restart loops and double counting. Older
manifests load with an empty evidence window.

Expose through existing RPC summaries and Inspector diagnostics:

- discovery trigger;
- duplicate evidence count and required count;
- distinct attempts;
- sampled piece count;
- current state, last check time, and sanitized error.

Normal logs should report state transitions and aggregate counts. Per-piece
Message-ID diagnostics remain debug-only.

## Failure And Restart Rules

- Restart resets interrupted discovery `Checking` to retryable `Error` and
  clears upload holds after repairing queued `Uploading` states.
- A manual request may bypass the automatic cooldown.
- A failed duplicate readback remains an upload failure and cannot trigger
  discovery.
- If BitTorrent never supplies a piece, automatic discovery may never trigger;
  this is intentional and the manual action is the escape hatch.
- If the provider retains a Message-ID tombstone without its article, the piece
  remains failed. Discovery must not hide this condition.

## Validation

Required tests:

1. Fresh metainfo does not automatically queue discovery or a remote integrity
   audit.
2. Raw `441`, repeated continuation `441`, and failed readback add no evidence.
3. Three distinct duplicate pieces with valid readback and sufficient ratio
   queue exactly one discovery.
4. Evidence from duplicate-content pieces is deduplicated by piece and base
   Message-ID semantics.
5. A low duplicate ratio does not trigger discovery.
6. Queue transition removes only the selected torrent's pending uploads and
   leaves no stale temporary files or `Uploading` states.
7. Successful discovery preserves verified/failed state, marks only unknown
   state optimistic, and starts integrity audit.
8. Failed discovery resumes normal local uploads.
9. Manual RPC works without evidence and rejects concurrent runs.
10. Web UI action enablement, progress, diagnostics, and RPC error handling are
    covered.
11. Restart recovery preserves evidence without double counting and clears
    interrupted holds.
12. Full CTest, Web UI lint/build, and a real-provider duplicate-upload test
    pass without logging credentials.

## Delivery Order

1. Structured Nyuu diagnostics and duplicate Message-ID classification.
2. Persistent evidence accounting and threshold state machine.
3. Upload queue hold, cancellation, and resume behavior.
4. Remove unconditional automatic discovery and fresh-manifest integrity
   probing.
5. Forced/manual discovery API and state-safe result merge.
6. RPC, Web UI context action, and diagnostics.
7. Focused, full-suite, and real-provider validation; README update and plan
   archival after merge.

## Acceptance Criteria

- Metainfo alone produces no automatic Usenet piece queries.
- Only distinct, hash-validated `441` duplicate pieces can automatically start
  discovery.
- The threshold stops redundant queued uploads without affecting other
  torrents.
- Manual discovery covers cold torrents with no upload evidence.
- Discovery never grants eviction credentials to unsampled pieces.
- Failure always resumes ordinary upload/BitTorrent behavior without stale
  queue or manifest state.
