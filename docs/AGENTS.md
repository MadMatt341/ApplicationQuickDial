# Documentation rules

Applies to `docs/`; root instructions also route root documentation edits here.

## Put each fact at its owning level

- Root `README.md`: purpose, install, common interaction, and links. Keep user configuration examples in `catalog.md`.
- Root `AGENTS.md`: product ownership, task routing, and completion requirements. Source/test rules belong in their scoped `AGENTS.md` files.
- `development.md` and `architecture.md`: short maps. Build commands live in the former; detailed system contracts live in `design/`.
- `testing.md`, `troubleshooting.md`, `releases.md`, and `performance.md`: references loaded for that task. Historical benchmark results must remain dated, not presented as current measurements.

## Maintain usefulness

- Give a detail document a clear task trigger and a path back to its map. Link to the owning section instead of copying it; repeat only brief summaries needed to route a reader.
- Preserve compatibility rules, failure behavior, limits, and meaningful verification when moving text. Do not turn a reorganization into a product or architecture change.
- Add a scoped `AGENTS.md` only for rules that apply to that directory; do not create empty levels or repeat inherited rules. Explicit task links provide more useful selection than deeper folders alone in this small repository.
- Prefer durable diagnosis procedures to incident transcripts. Keep machine-specific paths, SIDs, logs, and temporary plans in ignored output directories.
- Check local links and heading anchors, source paths, moved-section coverage, and `git diff --check`. Update incoming links after a move. No native build is needed for documentation-only edits.
- The release ZIP contains only the executable, README, and LICENSE. README links to user reference documents must work from the ZIP, using repository URLs where needed.
