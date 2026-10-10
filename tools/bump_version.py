#!/usr/bin/env python3
"""Next tracker version: minor + 1, as a commit of Tracker/VERSION and a tag.

The tracker's build takes its version from `git describe` (the latest tag shaped like 1.2.3, see
Tracker/CMakeLists.txt) and writes Tracker/VERSION; the trackers report it in their device info
packet, and tools/slime_link.py shows it. Run this once before each rebuild that gets flashed:

    python tools/bump_version.py        # 0.1.0 -> 0.2.0: commits Tracker/VERSION, tags 0.2.0

Commit everything else first: the tag goes on the commit that is built.
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
    major, minor = int(m.group(1)), int(m.group(2)) + 1
    version = f'{major}.{minor}.0'
    branch = git('branch', '--show-current') or 'unknown'
    # The same content the build generates from the tag, so building leaves the tree clean
    (ROOT / 'Tracker' / 'VERSION').write_text(
        f'VERSION_MAJOR = {major}\nVERSION_MINOR = {minor}\nPATCHLEVEL = 0\n'
        f'VERSION_TWEAK = 0\nEXTRAVERSION = {branch}\n', newline='\n')
    git('add', 'Tracker/VERSION')
    git('commit', '-q', '-m', f'Tracker {version}')
    git('tag', version)
    print(f'{last} -> {version} (commit and tag)')


if __name__ == '__main__':
    main()
