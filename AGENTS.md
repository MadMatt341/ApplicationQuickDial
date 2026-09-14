# Application Quick Dial

Native C++20 launcher for Windows 11 x64. Keep the Win32 design small; do not add a framework, service, installer, or cross-platform layer unless the task requires it.

## Ownership

The user owns behavior, interactions, appearance, and feel. The agent owns implementation, reliability, performance, and coverage within that behavior. Bring product changes and engineering tradeoffs that affect the experience to the user before implementing them. Routine internal fixes and verification do not require renewed approval.

## Load only what the task needs

- Product orientation: [README.md](README.md).
- Before source edits: [src/AGENTS.md](src/AGENTS.md), then use the [development map](docs/development.md#where-to-make-a-change) to select the affected reference sections.
- Before test edits: [tests/AGENTS.md](tests/AGENTS.md).
- Before documentation edits: [docs/AGENTS.md](docs/AGENTS.md), including root documentation.
- Building or packaging: [build setup](docs/development.md#prerequisites), [release workflow](docs/releases.md).
- Runtime diagnosis: the relevant section of [troubleshooting](docs/troubleshooting.md).

Do not preload all linked documents. Follow a deeper link when the task crosses that boundary; the source and tests remain the authority for current implementation.

## Completion

Run the CTest suite for every code change and the affected checks in [testing.md](docs/testing.md). For documentation-only changes, check links, preserved contracts, and `git diff --check`; no native rebuild is needed.

Keep generated content in `build/` or `out/`. Update the owning reference when changing a contract, shortcut, startup flow, message boundary, or build requirement. Keep task history and machine-specific diagnostic output out of permanent documentation.
