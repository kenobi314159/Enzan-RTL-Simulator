# `packaging/`

The recipes Enzan's packages are built from, one per distribution's own format, and the scripts
that build and test them. DESIGN_GUIDE.md section 11.7 is normative; `docs/releasing.md` says when
they run.

| File | What it is |
|---|---|
| `debian/` | Ubuntu 24.04 and Debian 13: copied into the unpacked tarball as its `debian/` |
| `rpm/enzan.spec` | Fedora, and RHEL 9 with gcc-toolset-13 |
| `arch/PKGBUILD` | Arch Linux |
| `build-package.sh` | builds one distribution's packages in a container of it, and tests them |
| `container.sh` | what `build-package.sh` runs inside the container, one phase at a time |
| `smoke-test.sh` | runs an installed package as a user would |

Every recipe builds from a release's source tarball, `scripts/make-dist.sh`'s, never from a
checkout or GitHub's archive, and with no network. Their `@...@` fields are filled in from the
tarball by `container.sh`, so the version is set only in `CMakeLists.txt`.

```sh
scripts/make-dist.sh
packaging/build-package.sh arch build/dist/enzan-*.tar.xz
```

builds the Arch packages into `build/packages/arch`. The targets are `ubuntu-24.04`, `debian-13`,
`fedora`, `el9` and `arch`.
