CI/CD workflow plan for NO_OS

Goal
Use CI/CD to prove that NO_OS still builds and passes its boot-time acceptance tests on every change. The workflow should be lightweight, reuse the existing PowerShell scripts, and return fast feedback for pull requests and main-branch updates.

Existing entry points
The repository already has build and test entry points in scripts/build.ps1:
- rebuild
- test
- run
- clean

The CI workflow should reuse these commands instead of introducing a separate build path.

Suggested triggers
- pull requests targeting main
- pushes to main
- manual workflow dispatch

Environment
- Windows runner
- Toolchain needed by the current scripts:
  - Zig
  - NASM
  - binutils, including objcopy
  - QEMU

Workflow steps
1. Check out the repository
2. Install the required toolchain
3. Run the rebuild step
4. Run the test step
5. Upload build artifacts and logs for debugging

Recommended commands
- pwsh -File scripts/build.ps1 rebuild
- pwsh -File scripts/build.ps1 test

What the workflow should verify
- The kernel builds successfully
- The boot-time test path completes without fatal errors
- The required artifacts are produced in the build directory
- Logs are preserved when a run fails

Artifacts to preserve
- build/kernel.elf
- build/kernel.elf64
- build/disk.img
- build/boot-test.log

Release planning
Release automation should be separate from PR validation. The release flow should:
1. Read the version from a single source of truth
2. Run the same build and test path used in CI
3. Update the changelog
4. Publish release artifacts
5. Create a GitHub release tag and notes

Implementation order
1. Add the CI validation workflow
2. Add artifact upload for successful and failed runs
3. Add release automation once CI is stable
4. Optionally add badges and branch protection rules later

Notes
This plan intentionally reuses the existing PowerShell-based build entry points and keeps the workflow simple and predictable for a freestanding OS project.
