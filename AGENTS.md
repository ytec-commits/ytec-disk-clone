# Project Safety and Licensing Rules

- The specification document is the highest-priority project requirement.
- Never write to a source disk.
- Never implement destructive disk I/O without explicit task scope and tests.
- Fail closed on unknown disk layouts, filesystems, encryption, or identifiers.
- Do not add kernel drivers, telemetry, networking, license-bypass, or BitLocker-bypass features.
- Do not copy or redistribute Microsoft WinPE/ADK/WIM/ISO/EXE/DLL files.
- Rescue media must be built locally from the user's installed ADK/WinPE add-on.
- Do not add GPL, AGPL, SSPL, LGPL, MPL, Commons Clause, BSL, or unknown-license dependencies.
- Any new dependency requires human approval and license documentation.
- Do not imitate proprietary cloning products or copy third-party code without provenance.
- Maintain THIRD-PARTY-NOTICES.txt and SBOM.spdx.json.
- Use stable disk identity checks; never trust disk number alone.
- All image inputs are untrusted and require bounds checking.
- Run build, tests, static analysis, and license checks before reporting completion.
