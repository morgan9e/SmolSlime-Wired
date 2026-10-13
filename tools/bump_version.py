#!/usr/bin/env python3
"""Next tracker version: patch + 1, as an annotated tag on the current commit.

The tracker's build takes its version from `git describe` (the latest tag shaped like 1.2.3, see
Tracker/CMakeLists.txt; it writes the git-ignored Tracker/VERSION). The trackers report it in their
device info packet, and tools/slime_link.py shows it. Run this once before each rebuild that gets
flashed:

    python tools/bump_version.py        # 0.0.3 -> 0.0.4: tags the current commit 0.0.4

Commit everything first: the tag goes on the commit that is built.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def main():
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('Commit or stash the changes first: the tag goes on the commit that is built.')
    try:
        last = git('describe', '--tags', '--abbrev=0', '--match', '[0-9]*.[0-9]*.[0-9]*')
    except subprocess.CalledProcessError:
        last = '0.0.0'
    m = re.fullmatch(r'v?(\d+)\.(\d+)\.(\d+)', last)
    if not m:
        sys.exit(f'Latest tag {last!r} is not a version')
    version = f'{m.group(1)}.{m.group(2)}.{int(m.group(3)) + 1}'
    git('tag', '-a', version, '-m', f'Tracker {version}')
    print(f'{last} -> {version} (tag on {git("rev-parse", "--short", "HEAD")})')


if __name__ == '__main__':
    main()
