---
name: release
description: >
  Create a new versioned release of lvt. Use this when asked to release,
  publish, or tag a new version. Determines the next version number,
  creates a git tag, and pushes it to trigger the release pipeline.
---

# Release lvt

## When to use

Use `/release` to publish a new version of lvt to GitHub Releases.

## Steps

1. **Check the latest tag** to determine the current version:

   ```bash
   git fetch --tags
   git tag --sort=-v:refname | head -5
   ```

2. **Ask the user** what kind of bump they want (patch, minor, or major), showing the current version and what each bump would produce.

3. **Verify the branch** — releases should only be created from `main`:

   ```bash
   git branch --show-current
   ```

   If not on `main`, warn the user and confirm before proceeding.

4. **Ensure working tree is clean**:

   ```bash
   git status --porcelain
   ```

   If there are uncommitted changes, stop and tell the user.

5. **Generate release notes** from the commits since the last tag:

   ```bash
   git log <PREV_TAG>..HEAD --oneline --no-decorate
   ```

   Summarize the commits into a human-readable changelog grouped by category:
   - **Bug fixes** — things that were broken and are now fixed
   - **New features** — new capabilities, CLI flags, providers
   - **Other changes** — refactors, docs, CI, etc.

   Omit empty categories. Write the notes in markdown. Show the draft to the user and ask for confirmation before proceeding.

6. **Create an annotated tag and push it**. Write the release notes into a temp file, then:

   ```bash
   git tag -a v<NEW_VERSION> -F <temp_file>
   git push origin v<NEW_VERSION>
   ```

   The release pipeline reads the tag message and uses it as the GitHub Release body.

7. **Confirm** by showing the user the tag and a link to the GitHub Actions run:
   `https://github.com/asklar/lvt/actions`

## Version format

Use semantic versioning: `vMAJOR.MINOR.PATCH` (e.g. `v0.2.0`).

- **patch**: bug fixes, minor tweaks
- **minor**: new features, new providers, new CLI flags
- **major**: breaking changes to output format or CLI interface
