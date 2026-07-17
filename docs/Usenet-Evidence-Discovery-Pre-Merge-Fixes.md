# Usenet Evidence Discovery Pre-Merge Fixes

Status: Resolved on `feature/usenet-evidence-driven-discovery`.

This document records the issues found during the pre-merge review after the
real-provider evidence-driven discovery roundtrip passed on 2026-07-17.

## Merge Blockers

### Interrupted Discovery Recovery

A daemon restart can leave a manifest in discovery `checking` after its worker
has disappeared. Startup must convert this state to a retryable error, preserve
the automatic retry cooldown, release upload holds, and resume eligible local
uploads. Automatic and manual discovery must not remain permanently blocked.

### Cooldown Evidence Window

An automatic discovery failure consumes its evidence window. Upload completions
during the persisted cooldown must not open or populate the next window. The
first eligible upload completion after cooldown expiry starts a fresh window;
manual discovery continues to bypass the cooldown.

### Integrity And Discovery Exclusion

Manual and automatic discovery must reject or defer while a full integrity
audit is `checking` or `repairing`. The two workers must not concurrently apply
independent manifest-wide state transitions.

## Efficiency Follow-Up

Upload cancellation currently affects only tasks still in the shared queue.
Workers may already have drained a large Nyuu batch into thread-local storage.
Batch construction and execution must observe the discovery hold closely enough
that reaching the evidence threshold avoids posting most remaining pieces.
Already submitted NNTP work may complete normally.

## Required Validation

1. Restart changes interrupted discovery to `error` and permits manual retry.
2. Restarted or failed discovery does not leave piece uploads permanently held.
3. Cooldown-period completions do not contribute evidence.
4. The first post-cooldown completion opens a new evidence window.
5. Discovery rejects integrity `checking` and `repairing` states.
6. Upload work that has not reached Nyuu is held once discovery starts.
7. Existing focused tests, full CTest, Web UI lint/build, and the real-provider
   evidence roundtrip remain valid.

## Resolution

- Startup converts interrupted discovery `checking` state to a retryable
  `error`, persists it, and resumes eligible local uploads.
- Upload completions inside `retry_after` do not populate a new evidence
  window. The first eligible completion at or after expiry starts it.
- Discovery rejects integrity `checking` and `repairing` states.
- Nyuu batches contain one torrent only and recheck discovery state before
  staging and immediately before submission. Held batches return to `unknown`;
  already submitted NNTP work is allowed to finish.
- Evidence is not updated while discovery is `checking` or already
  `available`, preventing repeated threshold processing from in-flight work.

Validation completed on 2026-07-17: 34 focused Usenet tests passed, all 629
enabled CTest tests passed (11 project-disabled tests were not run), and Web UI
lint/build passed. The real-provider evidence roundtrip was completed before
these fixes; this change set does not repeat external posts solely for merge
validation.
