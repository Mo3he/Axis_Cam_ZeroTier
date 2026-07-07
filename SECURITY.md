# Security Policy

This is an independent, community-developed ACAP package. It is provided on a
best-effort basis and is not an official Axis Communications product.

## Reporting a vulnerability

If you discover a security issue, please report it privately rather than opening
a public issue:

- Use GitHub's **[Report a vulnerability](../../security/advisories/new)**
  (Security > Advisories) to open a private advisory, or
- email **moshe@mohome.net** with the details.

Please include:

- the affected version (or `.eap` filename) and camera model / Axis OS version,
- a description of the issue and its impact, and
- steps to reproduce, if available.

You can expect an acknowledgement within a reasonable time. Fixes are shipped in
a new release; please avoid public disclosure until a fix is available.

## Scope

This VPN ACAP exposes proxy endpoints on the private overlay network (see the
README). Reports about the ACAP's own wrapper code, configuration handling, and
default settings are in scope. Vulnerabilities in the bundled upstream projects
(ZeroTierOne, libzt, lwIP) should also be reported upstream to their respective
projects.
