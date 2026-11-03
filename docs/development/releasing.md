# Making a release

What a maintainer does to release Enzan, in the order the steps are taken.

## Before

`main` is green and the packaging builds: run the release workflow by hand on `main` (Actions →
Release → Run workflow), which builds and tests every distribution's packages and releases nothing.
Locally, the same for one distribution is

```sh
scripts/make-dist.sh
packaging/build-package.sh ubuntu-24.04 build/dist/enzan-*.tar.xz
```

with Docker, or with `ENZAN_CONTAINER=podman`.

## The release

1. **The release commit.** In `CMakeLists.txt`, set `project(VERSION X.Y.Z)` and clear the
   default of `ENZAN_VERSION_SUFFIX` to `""`. Commit it as `build: Release X.Y.Z`, on `main`.
2. **The tag.** `git tag -a vX.Y.Z -m "Enzan X.Y.Z"` on that commit, and push it:
   `git push origin vX.Y.Z`.
3. **The workflow.** Pushing the tag runs the release workflow. It refuses a tag that is not the
   commit's version, so a tag on a `-dev` commit fails at once, and otherwise makes the tarball,
   builds and tests the packages, and creates a draft release with them attached.
4. **The draft.** Read the notes GitHub generated, say what the release is in a sentence or two
   above them, and publish it.
5. **The next version.** On `main`, set `project(VERSION ...)` to the next version and the suffix's
   default back to `"dev"`, as `build: Begin X.Y+1.0`.

## When the workflow fails

Nothing has been published. Fix the cause on `main`; then move the tag to the fixed commit, if the
version is unchanged, or release the next patch version. A published release is never replaced:
its tarball's checksum is in every package recipe that names it.
