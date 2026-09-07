# Security Policy

## Reporting a vulnerability

Please report security vulnerabilities privately to the maintainers
(`coldwings@me.com`) rather than opening a public issue. Include a
reproduction path, the affected component (format parsing, registry/DART
network paths, ublk data plane, supervisor process model), and any suggested
remediation.

## Threat model summary

- The block-device data plane treats image data as **untrusted input**:
  format parsers (`src/format/`) must bounds-check every header, index, and
  compression frame before use. Fuzzing format parsers is welcome.
- The supervisor spawns one isolated child process per device
  (`docs/supervisor.md`); a compromised or crashed device process must not
  affect sibling devices or the supervisor's control plane.
- Registry and DART proxy traffic is plain HTTP or TLS to operator-configured
  endpoints; credentials live in the config files described in
  `docs/config.md` and must never be logged.
- The ublk control surface requires privileges to create devices; the
  project never widens those privileges on its own (no setuid, no ambient
  capability acquisition).

See `docs/design-assumptions.md` for the full list of load-bearing
assumptions, including which sides of each boundary are trusted.
