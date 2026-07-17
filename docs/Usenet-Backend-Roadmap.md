# Usenet Backend Roadmap

Status: Active after the integrity repair milestone merged on 2026-07-17.

This document tracks remaining work that is not required for the current
experimental backend to operate, but matters for sustained production use. The
operator-facing behavior remains documented in
[Usenet Piece Backend README](Usenet-Piece-Backend-README.md).

## Priority 1: Bounded Local Storage

Implement a real capacity-pressure policy around `usenet_cache_size_mib` so a
long-running VPS converges on a configured local disk budget instead of relying
only on piece age. Prefer verified, cold, reconstructable pieces for eviction
and preserve actively served pieces. Report current cache bytes, target bytes,
eviction candidates, and blocked bytes through RPC and logs.

Extend safe reclamation beyond pieces contained in one file. Multi-file pieces
must only be reclaimed when every affected range can be punched safely; systems
without sparse-hole support need an explicit degraded state rather than silent
unbounded growth.

## Priority 2: Cold-Piece Serving

Improve the first request for an evicted piece. Coalesce concurrent peer
requests for the same hash, keep bounded request state while the Usenet fetch is
active, and resume service promptly after hash-verified restoration. Preserve
timeouts and normal BitTorrent fallback so slow or missing articles cannot hold
peer resources indefinitely.

## Priority 3: Long-Term Integrity

Add low-frequency, staggered audits for Ready torrents with conservative
provider query rates. Define a verification freshness window before destructive
eviction and use bounded exponential backoff for transient NNTP failures.
Permanent missing, malformed, or hash-mismatching chains must continue to be
withdrawn immediately and follow the existing repair path.

## Priority 4: Startup Probe Cost

The current startup check intentionally posts and reads both a small article and
the configured maximum payload. This validates the exact production path but
adds startup latency and leaves test articles at every launch. Add a securely
persisted provider/configuration fingerprint so a recent successful large probe
can be reused, while keeping credential authentication and a small read/write
check at startup. Provide an explicit force-full-probe option for deployment
validation.

## Priority 5: Automated End-to-End Coverage

Add a controlled NNTP fixture to CI for posting, duplicate Message-ID handling,
yEnc decoding, multipart assembly, missing continuation articles, corruption,
repair, restart recovery, and shared IO limits. Keep real-provider tests as a
manual release gate because credentials and provider behavior cannot be assumed
in public CI.

## Priority 6: Deployment And Platform Support

Make the amd64 VPS package either self-contained or able to report all required
runtime libraries before deployment. Add a repeatable Debian smoke test for the
packaged daemon, Web UI, Nyuu ABI, and RPC binding. Windows Usenet startup,
existence, and download paths remain unsupported and should only be implemented
after the Linux production path is stable.

## Completion Rules

Each item should be implemented on a focused branch with regression tests,
updated operator documentation, and a real runtime validation where applicable.
Completed plans move to `docs/archive/`; this roadmap should retain only active
or explicitly deferred work.
