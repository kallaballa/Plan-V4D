## Contributing guidelines

All guidelines for contributing to the OpenCV repository can be found at [`How to contribute guideline`](https://github.com/opencv/opencv/wiki/How_to_contribute).

## Code convention

Sources under `modules/plan/` and `modules/v4d/` follow [`CODE_CONVENTION.md`](CODE_CONVENTION.md).
The machine-checkable half of it (§2, formatting) is enforced by
`tools/format-check.sh`; install the git hook once with

```console
$ tools/install-hooks.sh
```

and every `git commit` will tell you if a staged source is off-convention and offer
to `clang-format` it. See [CODE_CONVENTION.md §2.3](CODE_CONVENTION.md#23-enforcement)
for the commands, the bypasses, and what the script deliberately does not check.
