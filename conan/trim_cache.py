#!/usr/bin/env python3
"""Keep the newest imported recipe and binary for each YSM build target.

Recipe identity is name@user/channel; versions and revisions compete by export
timestamp. Binary targets are (os, arch, build_type); settings-free packages
share one target. Stop cache writers (bootstrap, Conan and IDE builds) first.
"""

import argparse
from contextlib import closing
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile


EXPECTED_CONAN_VERSION = "2.32.0"


def storage_folder(cache):
    folder = cache / "p"
    config = cache / "global.conf"
    if config.is_file():
        for line in config.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.strip().partition("=")
            if separator and key.strip() == "core.cache:storage_path":
                folder = Path(value.strip())
    return folder.resolve()


def cache_path(storage, relative):
    path = (storage / relative).resolve()
    if not path.is_relative_to(storage) or path == storage:
        raise ValueError(f"Cache entry escapes storage folder: {relative}")
    return path


def read_cache(cache, *, ignore_conan_version=False):
    version = (cache / "version.txt").read_text(encoding="utf-8").strip()
    if not ignore_conan_version and version != EXPECTED_CONAN_VERSION:
        raise ValueError(f"Expected Conan {EXPECTED_CONAN_VERSION} cache, found {version}")
    storage = storage_folder(cache)
    database = storage / "cache.sqlite3"
    # The CLI lists recipes first and cannot discover orphan package records.
    with closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as connection:
        connection.row_factory = sqlite3.Row
        connection.execute("BEGIN")
        recipes = connection.execute(
            "SELECT reference, rrev, path, timestamp FROM recipes"
        ).fetchall()
        packages = connection.execute(
            "SELECT reference, rrev, pkgid, prev, path, timestamp FROM packages"
        ).fetchall()
    for entry in [*recipes, *packages]:
        cache_path(storage, entry["path"])
        timestamp = entry["timestamp"]
        if not isinstance(timestamp, (int, float)) or not math.isfinite(timestamp):
            raise ValueError(f"Invalid timestamp for {entry['reference']}")
        if timestamp <= 0:
            raise ValueError(f"Unfinished cache entry: {entry['reference']}; stop builds first")
    if any(not entry["pkgid"] or not entry["prev"] for entry in packages):
        raise ValueError("Unfinished package records; stop builds before trimming the cache")
    return version, storage, recipes, packages


def recipe_identity(reference):
    coordinate, _, namespace = reference.partition("@")
    name, separator, version = coordinate.partition("/")
    if not separator or not name or not version:
        raise ValueError(f"Invalid recipe reference: {reference}")
    return name, namespace


def build_target(info_path):
    settings = {}
    section = None
    for line in info_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif section == "settings":
            key, separator, value = line.partition("=")
            if not separator or not key.strip() or not value.strip():
                raise ValueError(f"Invalid settings in {info_path}: {line}")
            settings[key.strip()] = value.strip()
    if settings and ("os" not in settings or "arch" not in settings):
        raise ValueError(f"Cannot determine OS/architecture from {info_path}")
    return tuple(settings.get(key) for key in ("os", "arch", "build_type"))


def make_plan(storage, recipes, packages):
    latest_recipes = {}
    missing_recipes = set()
    for recipe in recipes:
        key = (recipe["reference"], recipe["rrev"])
        export = cache_path(storage, recipe["path"]) / "e"
        if not any((export / name).is_file() for name in ("conanfile.py", "conanfile.txt")):
            missing_recipes.add(key)
            continue
        identity = recipe_identity(recipe["reference"])
        rank = (recipe["timestamp"], *key)
        previous = latest_recipes.get(identity)
        if previous is None or rank > (previous["timestamp"], previous["reference"], previous["rrev"]):
            latest_recipes[identity] = recipe

    kept_recipes = {(entry["reference"], entry["rrev"]) for entry in latest_recipes.values()}
    removed_recipes = {(entry["reference"], entry["rrev"]) for entry in recipes} - kept_recipes
    removal_list = {}
    actions = []
    for reference, revision in sorted(removed_recipes):
        removal_list.setdefault(reference, {"revisions": {}})["revisions"][revision] = {}
        reason = "missing recipe files" if (reference, revision) in missing_recipes else "older recipe"
        actions.append(("recipe", f"{reference}#{revision}", reason))

    latest_packages = {}
    candidates = []
    for package in packages:
        key = (package["reference"], package["rrev"])
        if key in removed_recipes:
            # Conan removes these binaries together with their recipe.
            continue
        if key not in kept_recipes:
            candidates.append((package, "missing recipe"))
            continue
        info = cache_path(storage, package["path"]) / "p" / "conaninfo.txt"
        target = build_target(info)
        group = (*key, target)
        rank = (package["timestamp"], package["pkgid"], package["prev"])
        previous = latest_packages.get(group)
        if previous is None or rank > (previous["timestamp"], previous["pkgid"], previous["prev"]):
            latest_packages[group] = package
        candidates.append((package, "older binary for target"))

    kept_packages = {
        (entry["reference"], entry["rrev"], entry["pkgid"], entry["prev"])
        for entry in latest_packages.values()
    }
    for package, reason in candidates:
        reference, revision, package_id, package_revision = (
            package[key] for key in ("reference", "rrev", "pkgid", "prev")
        )
        if (reference, revision, package_id, package_revision) in kept_packages:
            continue
        recipe = removal_list.setdefault(reference, {"revisions": {}})["revisions"]
        binaries = recipe.setdefault(revision, {"packages": {}})["packages"]
        binaries.setdefault(package_id, {"revisions": {}})["revisions"][package_revision] = {}
        actions.append(("package", f"{reference}#{revision}:{package_id}#{package_revision}", reason))

    counts = (len(removed_recipes), len(packages) - len(kept_packages))
    return {"Local Cache": removal_list}, sorted(actions), counts


def run_conan(executable, cache, *arguments, capture=False):
    environment = os.environ.copy()
    environment["CONAN_HOME"] = str(cache)
    return subprocess.run(
        [executable, *arguments], env=environment, check=True,
        capture_output=capture, text=True, encoding="utf-8",
    )


def check_conan_version(executable):
    with tempfile.TemporaryDirectory(prefix="ysm-conan-trim-") as temporary:
        # Conan even migrates caches for --version; inspect it in an isolated home.
        output = run_conan(executable, Path(temporary) / "version-check", "--version", capture=True)
        match = re.fullmatch(r"Conan version (2\.\d+\.\d+)\s*", output.stdout)
        if not match or match[1] != EXPECTED_CONAN_VERSION:
            raise ValueError(
                f"Expected Conan {EXPECTED_CONAN_VERSION}; selected executable reports {output.stdout.strip()}"
            )


def apply_plan(executable, cache, plan):
    with tempfile.TemporaryDirectory(prefix="ysm-conan-trim-") as temporary:
        filename = Path(temporary) / "remove.json"
        filename.write_text(json.dumps(plan), encoding="utf-8")
        run_conan(executable, cache, "remove", "--list", str(filename), "--confirm")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(__file__).resolve().parent / "cache",
                        help="Conan home directory (default: conan/cache next to this script)")
    executable = parser.add_mutually_exclusive_group()
    executable.add_argument("--conan", default="conan", help="Conan executable (default: PATH conan)")
    executable.add_argument("--toolchain", type=Path, help="Use <toolchain>/tool/conan/bin/conan")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan without changing the cache")
    parser.add_argument("--ignore-conan-version", action="store_true",
                        help="Skip executable and cache version checks; Conan may migrate the cache")
    arguments = parser.parse_args()
    try:
        conan = arguments.conan
        if arguments.toolchain:
            suffix = ".exe" if os.name == "nt" else ""
            conan = str(arguments.toolchain.resolve() / "tool" / "conan" / "bin" / f"conan{suffix}")
        if not arguments.ignore_conan_version:
            check_conan_version(conan)
        cache = arguments.cache.resolve()
        _, storage, recipes, packages = read_cache(
            cache, ignore_conan_version=arguments.ignore_conan_version
        )
        plan, actions, counts = make_plan(storage, recipes, packages)
        print(f"Cache: {cache}")
        for kind, reference, reason in actions:
            print(f"Remove {kind}: {reference} ({reason})")
        print(f"Remove {counts[0]} recipe(s) and {counts[1]} binary package(s), including recipe cascades.")
        if arguments.dry_run:
            print("Dry run: no changes made.")
        elif actions:
            apply_plan(conan, cache, plan)
        return 0
    except (OSError, ValueError, sqlite3.Error, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
