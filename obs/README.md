# OBS packaging for Plan-V4D

Builds [Plan-V4D](https://github.com/kallaballa/Plan-V4D) — a custom build of
OpenCV 5.x with the Plan-DSL (`plan`) and V4D visualization (`v4d`) modules —
for four distros on the [Open Build Service](https://build.opensuse.org/):

| Target | Project | Repo | Packages |
|---|---|---|---|
| openSUSE Tumbleweed | `home:<user>:Plan-V4D:openSUSE_Tumbleweed` | `openSUSE_Tumbleweed` (openSUSE:Factory/standard) | RPM |
| Fedora | `home:<user>:Plan-V4D:Fedora` | `Fedora` (Fedora:Rawhide/rawhide) | RPM |
| Ubuntu 24.04 | `home:<user>:Plan-V4D:Ubuntu_24.04` | `Ubuntu_24.04` (Ubuntu:24.04/universe, x86_64 + aarch64) | deb |
| Raspbian 12 | `home:<user>:Plan-V4D:Raspbian_12` | `Raspbian_12` (Debian:12/standard, armhf + arm64) | deb |

RPM targets produce the subpackage set `plan-v4d-data`, `plan-v4d-libs`,
`plan-v4d-docs`, `plan-v4d-devel`, `plan-v4d-samples`; deb targets produce
`plan-v4d-data`, `plan-v4d-libs`, `plan-v4d-dev` (no `plan-v4d-docs` — the
plan/v4d guides ship inside `plan-v4d-dev`), and `plan-v4d-samples` — providing
the shared libraries, headers/pkgconfig, model data, docs, and `example_v4d_*`
demo binaries.

## Layout

```
obs/
├── plan-v4d.spec              # RPM spec (shared by Tumbleweed + Fedora)
├── packaging/ubuntu/          # deb packaging templates (debian.* + dsc.in)
├── packaging/raspbian/        # deb packaging templates (arm-specific)
├── home:<user>:Plan-V4D:*     # osc working copies (one per target)
├── results/<TARGET>/          # downloaded packages (osc-build.sh --results)
├── osc-setup.sh               # create OBS projects/packages + initial upload
├── regenerate.sh              # rebuild source tarballs, commit, trigger builds
├── osc-build.sh               # monitor / fetch / rebuild / submit
└── qemu-test/
    ├── qemu-test.sh           # boot real VMs & install+smoke-test the packages
    └── remote-test.sh         # test script executed inside each guest
```

## Workflow

1. **Set up** the OBS projects once:
   ```sh
   ./osc-setup.sh              # or: OSC_USER=me ./osc-setup.sh
   ```
   Requires `osc` installed and configured (run `osc ls` once so `~/.oscrc` or
   `~/.config/osc/oscrc` exists; `osc-setup.sh`/`osc-build.sh` read only the
   former, `regenerate.sh` reads both).

2. **Regenerate + upload sources** after any change to this repo or the OpenCV
   checkout (`$OPENCV_DIR`, branch `plan-v4d-5.x`):
   ```sh
   ./regenerate.sh             # build tarballs, commit to OBS, trigger rebuilds
   ./regenerate.sh --no-rebuild user
   OPENCV_DIR=/path/to/opencv VERSION=5.x.x~beta~kallaballa ./regenerate.sh
   ```
   This bypasses the broken `_service` (`osc`-gated `obs_scm`) and pushes source
   tarballs directly. RPM targets get the plain opencv + plan-v4d tarballs; deb
   targets get a merged opencv tarball with the Plan-V4D modules dropped in as
   `extra_modules/`.

3. **Monitor / fetch / rebuild / submit:**
   ```sh
   ./osc-build.sh                      # watch all four builds
   ./osc-build.sh --status             # one-shot status
   ./osc-build.sh --results            # download packages into results/<TARGET>
                                       # (only works once osc-build.sh's repo
                                       #  names match the project meta: Fedora
                                       #  and Raspbian_12, not standard/Debian_12)
   ./osc-build.sh --rebuild openSUSE_Tumbleweed
   ./osc-build.sh --submit openSUSE_Tumbleweed   # submit request to devel project
   ```

4. **Test the built packages** (see below).

## QEMU testing (`qemu-test.sh`)

The most thorough check: boot the **actual OBS-built packages** in real VMs of
each target distro and run an install + smoke-test suite inside them. This
catches what container builds can't — distro-specific installers, missing
runtime deps, ABI/linking issues, and GUI startup.

Base images are **downloaded automatically** on first use (needs `curl`; see
step 2 below), and the packages must already be in `obs/results/<TARGET>` before
you start: the check happens after the base image is downloaded and the VM has
booted, so a missing `results/<TARGET>` costs a full VM boot before it aborts.

```sh
./qemu-test.sh <target>       # tumbleweed | fedora | ubuntu | raspbian
./qemu-test.sh all            # run all four sequentially
./qemu-test.sh --stop fedora  # power off a running test VM
./qemu-test.sh --list-images  # show expected base images + paths
```

### How it works

1. **Packages** must already be in `obs/results/<TARGET>` (populate with
   `./osc-build.sh --results`): the check happens after the base image is
   downloaded and the VM has booted, so a missing `results/<TARGET>` costs a
   full VM boot before it aborts.
2. **Base images** live under `$WORK_ROOT/images` (default
   `/tmp/opencode/qemu/images`, override with `QEMU_WORK_ROOT`). If an image is
   missing it is **downloaded automatically** (needs `curl`):
   `tw.qcow2`, `fedora.qcow2`, `ubuntu.img`, `debian-arm64.qcow2`. Use
   `--list-images` to show what is looked up. Because the default `$WORK_ROOT`
   is the `/tmp` tmpfs, images are re-downloaded after a reboot; point
   `QEMU_WORK_ROOT` at a persistent directory to keep them.
3. **Boot**: each VM runs from a COW overlay over the pristine base image
   (resized to 8G). x86_64 targets use KVM (`-machine q35 -accel kvm -cpu host`);
   the aarch64 Raspbian target runs under TCG emulation (`-machine virt -bios
   QEMU_EFI.fd`). A cloud-init seed ISO (generated with `mkisofs`, ssh key +
   root password `v4dtest`) is attached as `-cdrom` for the *deb-less* targets
   and as a virtio-blk drive for Ubuntu/Raspbian, whose minimal kernels lack an
   IDE cdrom driver.
   The aarch64 target needs host support, which is auto-detected:
   `qemu-system-aarch64` and a `QEMU_EFI.fd` firmware. If missing the script
   prints how to install them (e.g. on openSUSE: `zypper in qemu-arm
   qemu-uefi-aarch64`).
4. **Drive via SSH** on per-target host ports: tumbleweed `2222`, fedora `2223`,
   ubuntu `2224`, raspbian `2225`. The script waits for ssh, copies the packages,
   then streams `qemu-test/remote-test.sh` into the guest.
5. **Tests inside the guest** (`remote-test.sh`, run as root):
   - install the `.rpm`/`.deb` packages with zypper/dnf/apt and verify the
     subpackages are present;
   - link-check every `example_v4d_*` binary via `ldd` (deploys a compiler +
     GL headers first);
   - `pkg-config --modversion opencv4` and compile+run a small OpenCV program
     that draws, writes, and re-reads an image;
   - ABI probe: locate `libopencv_v4d`/`libopencv_plan` via `ldconfig` and check
     `nm -D` for key exported symbols (`V4D::init`, `Plan::create`);
   - GUI smoke test: run `example_v4d_font_rendering` under Xvfb + Mesa
     `llvmpipe` for up to 90s (best-effort, skipped if `xvfb-run` is missing).

Exit code is the number of failed checks; a summary line prints
`RESULT: ALL TESTS PASSED` or `RESULT: N CHECK(S) FAILED`.

## Configuration

Environment variables honored by the scripts:

| Variable | Used by | Default |
|---|---|---|
| `OSC_USER` / `OBS_USER` | setup/regenerate/build | read from `~/.oscrc` |
| `OBS_API` | all osc commands | `https://api.opensuse.org` |
| `VERSION` | regenerate.sh | `5.x.x~beta~kallaballa` |
| `REVISION` | regenerate.sh | `1` |
| `OPENCV_DIR` | regenerate.sh | `<repo root>/opencv` (repo sibling) or cloned |
| `OPENCV_BRANCH` | regenerate.sh | `plan-v4d-5.x` |
| `PLANV4D_BRANCH` | regenerate.sh | `beta-5.x` |
| `FORCE_REBUILD` | regenerate.sh | `0` |
| `OPENCV_REPO_URL` | regenerate.sh | `https://github.com/kallaballa/opencv.git` |
| `PLANV4D_REPO_URL` | regenerate.sh | `https://github.com/kallaballa/Plan-V4D.git` |
| `QEMU_WORK_ROOT` | qemu-test.sh | `/tmp/opencode/qemu` |
