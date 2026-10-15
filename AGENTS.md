# Blade Launcher

Native C++20 launcher for Windows 11 x64. Keep the Win32 design small; do not add a framework, service, installer, or cross-platform layer unless the task requires it.

## Ownership

The user owns behavior, interactions, appearance, and feel. The agent owns implementation, reliability, performance, and coverage within that behavior. Bring product changes and engineering tradeoffs that affect the experience to the user before implementing them. Routine internal fixes and verification do not require renewed approval.

## Read only what the task needs

Use the task to select the references below.
For source changes, select the reference section in the [development map](docs/development.md#where-to-make-a-change).

- Product behavior: [README.md](README.md).
- Source changes: [source instructions](src/AGENTS.md), then the [development map](docs/development.md#where-to-make-a-change).
- Test changes: [test instructions](tests/AGENTS.md) and the related [test commands](docs/testing.md#targets-and-integration-commands).
- Documentation changes: [documentation instructions](docs/AGENTS.md), then [reading and writing rules](docs/documentation.md). Use these rules for root documents also.
- Builds: [prerequisites](docs/development.md#prerequisites) and [build commands](docs/development.md#configure-build-and-test).
- Packages: [release workflow](docs/releases.md).
- Runtime failures: [troubleshooting](docs/troubleshooting.md).

Read the selected section and its related source and tests.
Find the task's state owner, behavior, interfaces, dependencies, failure cases, compatibility rules, and test conditions.
If these facts are in a different reference, read the related sections there.
Stop when you have answers to the task questions.
Keep all necessary text in the selected section.

## Blade integration boundaries

For Blade integration or component extraction, follow the
[shell repository policy](https://github.com/MadMatt341/WindowManager/blob/main/docs/shell-architecture.md#repository-and-integration-policy) and
[Launcher's architecture](docs/architecture.md#blade-integration).
Keep Launcher independently usable and versioned. A Rail launcher widget is
an adapter to Launcher; launcher state and behavior remain here. Separate a
helper process when its lifetime or failure isolation requires it, without
creating a repository solely because it is executable.

## ASD-STE100

Write all new documentation in ASD-STE100 Simplified Technical English, Issue 9.
Use this standard when you change documentation.
Obey the writing rules and dictionary requirements in
[ASD-STE100 Simplified Technical English, Issue 9](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf).
Before you complete a documentation change, examine all new and changed text.

## Completion

Run the CTest suite for every code change and the affected checks in [testing.md](docs/testing.md). For documentation-only changes, check links, preserved contracts, and `git diff --check`; no native rebuild is needed.

Keep generated content in `build/` or `out/`. Update the owning reference when changing a contract, shortcut, startup flow, message boundary, or build requirement. Keep task history and machine-specific diagnostic output out of permanent documentation.

## Completion reports

When changes are ready for a commit, push, or product release, give the [completion report](https://github.com/MadMatt341/WindowManager/blob/main/docs/blade-development.md#completion-report).
Include each state and any remaining action.
Keep the task approvals.

## Finish and sync Blade

For usual Blade development, use `main`.
Use a different branch for a test.
When the user says "finish and sync", follow the [Blade completion procedure](https://github.com/MadMatt341/WindowManager/blob/main/docs/blade-development.md#finish-and-sync).
This request gives approval for checks, commits, pushes to main, signing, publication, and next-start staging across Blade repositories.
Publication is the default after the normal release checks pass.
Use source-only completion when the user requests it.
Keep work outside the task scope.
Do not sign out or restart the running shell.

When the user says "continue here", use the [Blade continuation procedure](https://github.com/MadMatt341/WindowManager/blob/main/docs/blade-development.md#continue-here).
This request gives approval to get saved source and change clean checkouts to its recorded revisions.
When the user says "save my work", use the [Blade work checkpoint procedure](https://github.com/MadMatt341/WindowManager/blob/main/docs/blade-development.md#save-my-work).
This request gives approval to save unfinished files and staged versions in private Git storage.
Keep the original working files and index.
Neither request gives approval for product publication or a shell restart.
