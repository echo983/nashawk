# Nashawk Usenet Web UI Plan

Status: Active design plan

## Goal

Add a small, usable Usenet observability surface to the existing Transmission Web UI with the least practical disruption to the upstream interface.

The first version is read-only. It should make it clear whether the daemon is running in Usenet mode, whether local piece eviction is enabled, how much Usenet IO capacity is configured and currently in use, and whether a selected torrent has enough Usenet-backed pieces to be served after local piece eviction.

This work is intentionally not a full storage dashboard and not a secret-management UI.

## Current UI Shape

The Web UI is a vanilla JavaScript application under `web/src/`.

High-value extension points:

- `web/src/statistics-dialog.js`: already polls `session_stats` every five seconds and is a good place for daemon-level read-only runtime status.
- `web/src/prefs-dialog.js`: already shows daemon settings from `session_get`; useful later for editable Usenet settings, but heavier for a first read-only cut.
- `web/src/inspector.js`: already refreshes selected torrent details every three seconds and is the best place for per-torrent Usenet piece summary.
- `web/src/torrent.js`: owns torrent field storage and extra field lists; adding accessors here keeps UI rendering simple.
- `web/src/remote.js`: generic RPC wrapper already supports `session_get`, `session_stats`, and `torrent_get`.

## Backend/RPC Gap

The backend already has the core Usenet state:

- session settings: `usenet_enabled`, `usenet_upload_concurrency`, `usenet_eviction_enabled`, `usenet_eviction_min_age_minutes`, `usenet_cache_size_mib`
- runtime queues and IO limiter internals in `tr_session`
- per-torrent manifests in `tr_usenet_piece_store`
- per-piece states: `unknown`, `uploading`, `available`, `failed`

The Web UI does not yet have stable RPC fields for the runtime queue/IO state or per-torrent manifest summaries. Adding those read-only fields is the correct first backend step.

## Proposed RPC Additions

Use snake-case RPC field names to match the current Nashawk settings keys.

### `session_get`

Expose existing safe settings so the Web UI can show configuration without reading secrets:

- `usenet_enabled`
- `usenet_upload_concurrency`
- `usenet_eviction_enabled`
- `usenet_eviction_min_age_minutes`
- `usenet_cache_size_mib`

These values are not secrets. Do not expose server host, port, username, group, or password from `.env`.

### `session_stats`

Add a read-only `usenet` object:

```json
{
  "usenet_enabled": true,
  "usenet_io_limit": 40,
  "usenet_io_active": 3,
  "usenet_upload_queue_size": 12,
  "usenet_download_queue_size": 1,
  "usenet_download_in_flight": 1,
  "usenet_upload_concurrency": 40,
  "usenet_eviction_enabled": true,
  "usenet_eviction_min_age_minutes": 60,
  "usenet_cache_size_mib": 4096
}
```

Implementation notes:

- Values must be sampled under the existing mutexes.
- `usenet_io_active` must count both upload and download work because both share the same limiter.
- No `.env` values are exposed.
- If Usenet mode is disabled, still return the object with `usenet_enabled: false` and zero runtime counters. This makes UI and diagnostics stable.

### `torrent_get`

Add one read-only field, `usenet_piece_summary`, returned as an object:

```json
{
  "eligible": true,
  "manifest_present": true,
  "piece_count": 128,
  "local_piece_count": 8,
  "unknown": 0,
  "uploading": 2,
  "available": 126,
  "failed": 0,
  "servable": 128
}
```

Semantics:

- `eligible`: torrent has metainfo and the piece size fits the configured one-article-per-piece policy.
- `manifest_present`: the manifest exists and was readable.
- `piece_count`: torrent piece count.
- `local_piece_count`: currently verified local pieces.
- `unknown`, `uploading`, `available`, `failed`: manifest state counts.
- `servable`: count of pieces available locally or via available Usenet manifest state.

For multi-file, cross-file, and hole-punch-limited torrents, this field remains about serving capability, not eviction eligibility. Eviction-specific constraints can be added later if needed.

## Web UI First Cut

### Daemon Status

Add a `Usenet` section to the existing Statistics dialog.

Show:

- Mode: enabled / disabled
- IO: active / limit
- Upload queue
- Download queue and in-flight downloads
- Eviction: enabled / disabled
- Eviction minimum age
- Local cache target

Reasoning: the Statistics dialog already exists, already refreshes periodically, and is naturally read-only. This avoids adding editable controls before the runtime behavior is visible and tested.

### Per-Torrent Status

Add a `Usenet` section to the Inspector `Info` tab.

For one selected torrent, show:

- Eligibility
- Servable pieces: `servable / piece_count`
- Manifest: present / missing
- Local pieces
- Available / Uploading / Failed / Unknown counts

For multiple selected torrents, show `Mixed` or aggregate counts only if the aggregation is trivial and clear. First implementation may use `Mixed` to keep behavior predictable.

Do not add row badges in the first cut. Row badges are visually louder, require more list refresh testing, and are not required to debug the feature.

## Debug And Codex-Friendly Diagnostics

The UI should make status easy to copy into a chat or issue without exposing secrets.

First cut:

- Keep stable JSON field names in RPC.
- Add a small `Copy diagnostics` button in the Statistics dialog Usenet section.
- The copied JSON should include the `session_stats.usenet` object and a timestamp.
- For selected torrent diagnostics, add a similar copy button only if it can be implemented without cluttering the Inspector. Otherwise defer it and rely on RPC examples in docs.

Debug logging:

- Add a local Web UI preference such as `usenet-debug-logs` stored in browser preferences or `localStorage`.
- When enabled, log Usenet RPC snapshots to the browser console with a stable prefix: `[nashawk-usenet]`.
- Do not enable verbose logs by default.

Backend logs:

- Continue using existing trace/warn logging for upload, download, restore, and eviction.
- Add concise trace logs only where new RPC summary generation fails to read a manifest or encounters an unexpected state.

## Security Boundaries

- Never expose `.env` content through RPC or Web UI.
- Never show Usenet username, password, server, port, or group in diagnostics.
- First UI version is read-only.
- Do not add destructive controls such as `evict now`, `reupload`, or `forget manifest`.
- Do not enable or disable Usenet mode from the Web UI in this cut. Startup validation is intentionally a daemon startup responsibility.

## Implementation Phases

1. Add backend snapshot helpers.
   - Add a small session-level Usenet runtime snapshot method.
   - Add a torrent-level Usenet summary method.
   - Keep locking local and short.

2. Add RPC fields.
   - Extend quarks for `usenet`, `io_limit`, `io_active`, queue sizes, and `usenet_piece_summary`.
   - Add `session_stats.usenet`.
   - Add `torrent_get` field `usenet_piece_summary`.
   - Add RPC tests for disabled mode and enabled-mode shape where practical.

3. Add minimal Web UI.
   - Statistics dialog `Usenet` section.
   - Inspector Info `Usenet` section.
   - Torrent accessors and field refresh lists.
   - Copy diagnostics button for daemon status.

4. Add debug affordances.
   - Browser-side debug preference/localStorage flag.
   - Stable `[nashawk-usenet]` console output when enabled.
   - Document one or two direct RPC payloads that Codex can ask the user to run.

5. Validate.
   - `./code_style.sh --check`
   - targeted RPC/unit tests
   - full `ctest --test-dir build --output-on-failure`
   - `npm`/Web build if local dependencies are present
   - manual Web UI smoke test against a daemon with Usenet enabled

## Non-Goals For This Branch

- Editing `.env` or Usenet credentials in the browser.
- Full per-piece browser.
- Torrent row badges or new global dashboard.
- Manual repair buttons.
- NZB support.
- Changing the one-BT-piece-to-one-Usenet-article policy.

## Merge Criteria

The branch can be considered for merge when:

- RPC returns safe, stable Usenet daemon and torrent status fields.
- Web UI displays daemon and selected torrent Usenet state without breaking the existing dialogs.
- Diagnostics can be copied without secrets.
- Existing tests pass.
- At least one real torrent is checked through the UI after local piece eviction, confirming that the UI reports Usenet-backed serving state correctly.
