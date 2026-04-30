# Security Policy

## Supported Versions

Supported versions are documented through tagged releases, release notes,
`CHANGELOG.md`, and package metadata. Security fixes target maintained release
lines only.

## Reporting a Vulnerability

Use the `Report a security vulnerability` issue template in this repository.
Include:

- affected version or commit
- impact summary
- reproduction details if safe to share
- suggested mitigation if known

We will triage the report and prioritize a fix according to impact and
exploitability.

## Security Updates

When a fix is accepted, we will:

- patch the affected maintained release line
- publish an updated gem release when appropriate
- document the fix in release notes and `CHANGELOG.md`

Version selection follows semantic versioning:

- patch release for backward-compatible security fixes
- minor release for larger compatible security work
- major release for breaking remediation when unavoidable

## Code of Conduct Abuse Reports

If you believe someone is violating the Code of Conduct, report it to
<admin@codevedas.com>.
