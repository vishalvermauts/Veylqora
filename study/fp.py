"""Fingerprint of everything in a click tree except src/click (the code under test).

Used to detect changes to tests, configuration, conftest files or new files
outside the code the agent is allowed to edit. Usage: fp.py <root>
Prints one hex digest. Run it as root on the pristine tree and as the agent on
the working copy; the two digests must match before the agent starts.
"""

import hashlib
import pathlib
import sys

EXCLUDED_PARTS = {"__pycache__", ".pytest_cache", ".egg-info"}


def fingerprint(root: pathlib.Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if rel.parts[:2] == ("src", "click"):
            continue
        if EXCLUDED_PARTS & set(rel.parts) or any(p.endswith(".egg-info") for p in rel.parts):
            continue
        if path.is_dir():
            continue
        digest.update(f"{rel.as_posix()}\0".encode())
        if path.is_symlink():
            digest.update(f"LINK:{path.readlink()}\0".encode())
        else:
            digest.update(path.read_bytes())
    return digest.hexdigest()


if __name__ == "__main__":
    print(fingerprint(pathlib.Path(sys.argv[1])))
