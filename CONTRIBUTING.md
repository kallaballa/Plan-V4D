## Contributing guidelines

All guidelines for contributing to the OpenCV repository can be found at [`How to contribute guideline`](https://github.com/opencv/opencv/wiki/How_to_contribute).

## Code convention

Sources under `modules/plan/` and `modules/v4d/` follow the project's coding
conventions — `.clang-format` for everything clang-format can express, plus the
hand-written rules it cannot. The machine-checkable half of those rules
(§2, formatting) is enforced by `tools/format-check.sh`; install the git hook
once with

```console
$ tools/install-hooks.sh
```

and every `git commit` will tell you if a staged source is off-convention and offer
to `clang-format` it. Any violation is a question rather than a veto — existing
files are grandfathered, and `b` always gets you out.

```console
$ SKIP_FORMAT_CHECK=1 git commit ...    # bypass once
$ tools/format-check.sh --staged        # check only
$ tools/format-check.sh --all           # audit every source
$ tools/format-check.sh --fix ...       # rewrite the offenders in place
```

`tools/format-check.sh` reports and can fix the formatting rules; naming, class
layout, state and schedule rules are review rules and are checked by a human.
See the script's own header for the commands, the bypasses, and what it
deliberately does not check.

> The rule taxonomy these scripts cite as `CODE_CONVENTION.md` (§2 formatting,
> §17 and so on) is not part of this repository: the `§` numbers in
> `tools/format-check.sh` and in the pre-commit hook refer to that document,
> which is why the sections above describe what is actually enforced here
> rather than restating its numbering.
