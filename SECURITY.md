# Security policy

## Supported version

Security fixes are made against the current version on the default branch. Use the latest executable published from this repository and verify that it came from `LIN4CRE/Youtube-Lyrics`.

## Reporting a vulnerability

Please do **not** publish exploit details in a public issue. Use GitHub's **Security → Report a vulnerability** flow for this repository when it is available. If that option is not enabled for you, contact the maintainer privately through the [LIN4CRE GitHub profile](https://github.com/LIN4CRE) and ask for a private reporting route before sending technical details.

This is a small, volunteer-maintained project; no response-time or coordinated-disclosure SLA is promised. Please give the maintainer a reasonable opportunity to investigate and prepare a fix before public disclosure.

## Scope notes

The application reads Windows media-session metadata and contacts LRCLIB and lyrics.ovh for lyric lookups. It does not capture audio or send telemetry. A security report should include the affected app version, Windows version, impact, and minimal reproduction steps—without posting private track history or unredacted logs publicly.
