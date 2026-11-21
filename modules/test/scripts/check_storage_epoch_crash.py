"""Check epoch initialization crashes with the production storage test binary."""

import argparse
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile


def run_child(binary: Path, scope: Path, test: str) -> subprocess.CompletedProcess:
    environment = os.environ.copy()
    environment["YSM_STORAGE_TEST_EXIT_SCOPE"] = str(scope)
    return subprocess.run(
        [str(binary), f"--gtest_filter=StorageEpochProcessChild.{test}"],
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )


def check(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="ysm-storage-crash-") as directory:
        scope = Path(directory)
        crashed = run_child(binary, scope, "ExitDuringInitialization")
        assert crashed.returncode == 72, crashed.stdout + crashed.stderr
        checked = run_child(binary, scope, "VerifyInterruptedInitialization")
        assert checked.returncode == 0, checked.stdout + checked.stderr
        with closing(sqlite3.connect(scope / "preferences-c2.sqlite")) as target:
            assert target.execute("PRAGMA integrity_check").fetchone() == ("ok",)
            assert target.execute("SELECT count(*) FROM sqlite_schema").fetchone() == (0,)
            assert target.execute("PRAGMA application_id").fetchone() == (0,)
            assert target.execute("PRAGMA user_version").fetchone() == (0,)
        print("PASS: interrupted commit leaves no partial initialization; residue rejected")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("test_binary", type=Path)
    arguments = parser.parse_args()
    binary = arguments.test_binary.resolve(strict=True)
    check(binary)


if __name__ == "__main__":
    main()
