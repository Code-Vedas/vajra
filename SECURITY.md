# Security Policy

## Supported Versions

Only the latest released Vajra version receives security fixes. Earlier versions are unsupported; upgrade to the latest release before requesting a security backport. The current release is identified by RubyGems, repository tags, and `CHANGELOG.md`.

## Reporting a Vulnerability

Report vulnerabilities privately through [GitHub Security Advisories](https://github.com/Code-Vedas/vajra/security/advisories/new). Do not open a public issue for a suspected vulnerability. Include:

- affected version or commit
- impact summary
- reproduction details
- suggested mitigation if known

We will triage the report and prioritize a fix according to impact and exploitability.

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
