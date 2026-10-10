# Documentation policy

This guide gives reading and writing rules for this repository.
Policy version: `0.1.0`.
Read it when you change documents, their structure, or contributor instructions.
Use the reading rules when source, tests, or documentation are necessary for a task.

## Read the related references

Start with the task links in [AGENTS.md](../AGENTS.md#read-only-what-the-task-needs).
For a source change, use the [development map](development.md#where-to-make-a-change) to select the reference section.
A reference owner is the document that gives the current rules for a subject.
Read the selected section and its related source and tests.
Read the conditions and limits in the paragraphs before and after the selected text.
Do not use a search result or a line limit as the full reference.

Before you make a decision, find answers to the task questions:

- Which component changes and keeps the state?
- Which behavior must the change keep?
- Which interface or dependency changes?
- Which effects occur after an operation failure?
- Which compatibility rules must the change obey?
- Which tests give evidence for this behavior?
- Which environment and prerequisites are necessary for those tests?

If an answer is in a different reference, read the related section in that reference.
If the source and the reference give different information, examine the difference.
Stop when the related references, source, and tests give answers to the task questions.
Read more if new evidence changes those questions.
Read a different document only when it gives information that is necessary for the task.
Keep all necessary text in the selected section.

The [verification reference](testing.md#targets-and-integration-commands) gives test commands and limits.
Select only the [manual verification](testing.md#manual-verification) sections about the change.
Before you do a desktop test again after a failure, read [startup and desktop](troubleshooting.md#startup-and-desktop).
An automated check does not always show the Windows effect.
Examine Windows behavior independently of the test program.

## Write at the reference owner

Use [docs/AGENTS.md](AGENTS.md#put-each-fact-at-its-owning-level) to select the document for each fact.
Keep current behavior, commands, prerequisites, and limits before dated evidence.
Keep one reference owner for each fact.
Give links to that owner.
Do not give the same rules in other references.
Include only the information that is necessary for the task.

When you change behavior, change its reference owner in the same change.
Then examine the overview, task links, commands, and examples that are related to the change.
Correct current statements when new evidence shows that they are incorrect.
Keep dated results with their build, environment, result, and remaining checks.
Put raw logs and generated reports in `build/` or `out/`.

Give procedures their prerequisites, full command text, expected results, and cleanup instructions.
Show effects on the active desktop or installation files when those effects occur.
During a language review, do not change commands, code, paths, identifiers, API names, UI labels, or quotations.
Write the text about these items in STE.

Use these word counts as examination targets:

| Document or section | Examination target |
|---|---|
| Contributor guide such as `AGENTS.md` | 500 to 800 words. Examine a guide with more than 1,000 words. |
| Routing map | 100 to 200 words. |
| Section about one subject | 300 to 800 words. |

A routing map shows the reference sections for each task.
These targets are not maximum word counts.
Do not add words only to increase a word count.
Do not remove a contract, failure case, or test condition to decrease a word count.

If a different document is necessary for a subject, put it at that subject's reference owner.
Give a link to it from the task map with the related task.
Such a document is a child reference.
Keep its scope and its return link clear.
Do not divide a document only because its word count is more than a target.
The development and architecture maps give task links to the design references.

Before you complete a documentation change, examine all new and changed text.
Examine relative links and heading anchors that the change adds or changes.
Use `git diff --check`.
Examine changed commands against their source when the task changes those commands.
Record each check that you cannot complete.
A native rebuild is not necessary for a documentation change.

## ASD-STE100

Write all new documentation in ASD-STE100 Simplified Technical English, Issue 9.
Use this standard when you change documentation.
Obey Part 1 (writing rules) and Part 2 (dictionary) of the
[official Issue 9 PDF](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf).

Use approved dictionary words only with their approved meanings, parts of speech, and forms.
Use technical nouns and technical verbs only as permitted by the standard.
Use the full writing rules and dictionary to examine the text.
If you cannot complete these checks, record the cause in the task result.

## Change this policy

Change these rules only to correct a problem that the task evidence shows.
Record the problem, the rule change, and the evidence.
Keep the change related to that problem.
Before you use a new policy version here, record that version in this guide.
This guide contains the rules that this repository uses.
