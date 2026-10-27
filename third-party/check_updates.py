#!/usr/bin/env python3
"""Check Conan source references without changing recipes or cloning sources.

Requires Python 3.10+, Git, PyYAML and packaging. GITHUB_TOKEN (or GH_TOKEN)
and GITLAB_TOKEN are optional API credentials. Exit 1 means a check failed;
available updates and explicitly skipped sources do not cause failure.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, urlopen

try:
    import yaml
    from packaging.version import InvalidVersion, Version
except ImportError:
    sys.exit("Install dependencies: python -m pip install PyYAML packaging")


@dataclass(frozen=True)
class Repository:
    host: str
    path: str
    provider: str

    @property
    def url(self):
        return f"https://{self.host}/{self.path}"

    @property
    def api(self):
        if self.provider == "github":
            return f"https://api.github.com/repos/{self.path}"
        return f"https://{self.host}/api/v4/projects/{quote(self.path, safe='')}"


def repository_from_url(url, gitlab_hosts=()):
    if re.match(r"^[\w.-]+@[^/]+:", url):
        url = "ssh://" + url.replace(":", "/", 1)
    parsed = urlsplit(url)
    host = (parsed.hostname or "").lower()
    path = parsed.path.strip("/").removesuffix(".git")
    parts = path.split("/")
    if parsed.scheme not in {"https", "http", "ssh", "git"} or parsed.query or parsed.fragment:
        return None
    if not all(parts) or any(part in {".", "..", "-"} for part in parts):
        return None
    if host == "github.com" and len(parts) == 2:
        return Repository(host, path, "github")
    if host in {"gitlab.com", *gitlab_hosts} and len(parts) >= 2:
        return Repository(host, path, "gitlab")
    return None


def run_git(args, timeout, cwd=None):
    environment = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never")
    result = subprocess.run(
        ["git", *args], cwd=cwd, env=environment, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=timeout,
    )
    if result.returncode:
        raise ValueError(result.stderr.strip() or f"Git exited with {result.returncode}")
    return result.stdout


def recipe_paths(root, timeout):
    # Git applies ignore rules before enumerating untracked directories.
    output = run_git(
        ["ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "*/conandata.yml"],
        timeout, cwd=root,
    )
    return sorted({root / name for name in output.split("\0") if name})


def read_sources(path):
    # BaseLoader preserves date tags and numeric-looking version keys as strings.
    data = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    if not isinstance(data, dict) or not isinstance(data.get("sources"), dict) or not data["sources"]:
        raise ValueError("Expected a non-empty sources mapping")
    for version, source in data["sources"].items():
        name = f"{path.parent.name}/{version}"
        if not isinstance(source, dict):
            raise ValueError(f"{name}: expected a source mapping")
        yield name, source
        if str(source.get("local", "false")).lower() == "true":
            continue
        extras = source.get("extra_git", [])
        if not isinstance(extras, list):
            raise ValueError(f"{name}: expected an extra_git list")
        for index, extra in enumerate(extras):
            if not isinstance(extra, dict):
                raise ValueError(f"{name}: expected an extra_git mapping")
            yield f"{name}:{extra.get('target', index)}", extra


def tag_version(tag):
    match = re.fullmatch(r"([^0-9]*)(\d+(?:[._-]\d+)*)(.*)", tag)
    if not match:
        return None
    prefix, release, suffix = match.groups()
    try:
        version = Version(re.sub(r"[_-]", ".", release) + suffix)
    except InvalidVersion:
        return None
    return ("" if prefix.lower() == "v" else prefix.lower()), version


def check_tag(repository, tag, timeout):
    current = tag_version(tag)
    if current is None:
        raise ValueError(f"Cannot order tag {tag!r} as a version")
    output = run_git(["ls-remote", "--tags", "--refs", repository.url], timeout)
    tags = [line.split("\t", 1)[1].removeprefix("refs/tags/") for line in output.splitlines()]
    if tag not in tags:
        raise ValueError(f"Current tag {tag!r} does not exist upstream")
    newer = []
    for candidate in tags:
        parsed = tag_version(candidate)
        if parsed and parsed[0] == current[0] and parsed[1] > current[1]:
            newer.append((parsed[1], candidate))
    if newer:
        latest_version, latest = max(newer)
        qualifier = " (prerelease)" if latest_version.is_prerelease or latest_version.is_devrelease else ""
        return "UPDATE", f"tag {tag} -> {latest}{qualifier}; {len(newer)} newer tag(s)"
    return "OK", f"tag {tag}; no newer tag in the same version family"


def get_json(repository, endpoint, timeout):
    headers = {"Accept": "application/json", "User-Agent": "ysm-dependency-update-checker"}
    if repository.provider == "github":
        token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        if token:
            headers["Authorization"] = f"Bearer {token}"
    else:
        token = os.environ.get("GITLAB_TOKEN")
        if token:
            headers["PRIVATE-TOKEN"] = token
    request = Request(repository.api + endpoint, headers=headers)
    try:
        with urlopen(request, timeout=timeout) as response:
            return json.load(response)
    except HTTPError as error:
        error.close()
        hint = " Check API credentials or rate limits." if error.code in {401, 403, 429} else ""
        raise ValueError(f"API HTTP {error.code} at {request.full_url}.{hint}") from error


def check_revision(repository, revision, branch, timeout):
    if not re.fullmatch(r"[0-9a-fA-F]{7,40}", revision):
        raise ValueError(f"Expected a commit SHA, found {revision!r}")
    default = not branch
    if default:
        branch = get_json(repository, "", timeout)["default_branch"]
    if not branch:
        raise ValueError("Repository has no default branch")
    branch_label = f"branch {branch}" + (" (assumed remote default)" if default else "")
    encoded_branch = quote(branch, safe="")
    if repository.provider == "github":
        head = get_json(repository, f"/branches/{encoded_branch}", timeout)["commit"]["sha"]
        comparison = get_json(repository, f"/compare/{revision}...{head}?per_page=1", timeout)
        if comparison["status"] == "identical":
            return "OK", f"{branch_label}; revision {revision} is at HEAD {head}"
        if comparison["status"] != "ahead":
            raise ValueError(f"{branch_label}: revision {revision} is not an ancestor of HEAD {head}")
    else:
        head = get_json(repository, f"/repository/branches/{encoded_branch}", timeout)["commit"]["id"]
        current = get_json(repository, f"/repository/commits/{revision}", timeout)["id"]
        if head == current:
            return "OK", f"{branch_label}; revision {revision} is at HEAD {head}"
        query = urlencode([("refs[]", current), ("refs[]", head)])
        base = get_json(repository, f"/repository/merge_base?{query}", timeout)["id"]
        if base != current:
            raise ValueError(f"{branch_label}: revision {revision} is not an ancestor of HEAD {head}")
    return "UPDATE", f"{branch_label}; revision {revision} -> {head} (ancestor verified)"


def check_source(source, args):
    if str(source.get("local", "false")).lower() == "true":
        return "SKIP", "local source; no remote repository"
    url = source.get("url")
    if not isinstance(url, str) or not url:
        raise ValueError("Missing source URL")
    repository = repository_from_url(url, args.gitlab_host)
    if repository is None:
        return "SKIP", f"not a recognized GitHub/GitLab repository URL: {url}"
    if source.get("tag"):
        return check_tag(repository, str(source["tag"]), args.timeout)
    revision = source.get("commit") or source.get("revision")
    if not revision:
        raise ValueError("Missing tag or commit/revision")
    return check_revision(repository, str(revision), source.get("branch"), args.timeout)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*", help="Optional dependency directory names; default: all")
    parser.add_argument("--timeout", type=float, default=30, help="Timeout per Git/API request in seconds (default: 30)")
    parser.add_argument("--gitlab-host", action="append", default=[], metavar="HOST", help="Additional GitLab host; repeatable")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and greater than zero")
    args.gitlab_host = [host.lower() for host in args.gitlab_host]
    counts = Counter()

    def report(status, name, detail):
        counts[status] += 1
        print(f"[{status}] {name}: {detail}", flush=True)

    try:
        paths = recipe_paths(Path(__file__).resolve().parent, args.timeout)
        if not paths:
            raise ValueError("No conandata.yml files found")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report("ERROR", "discovery", str(error))
        return 1
    for name in sorted(set(args.names) - {path.parent.name for path in paths}):
        report("ERROR", name, "unknown dependency")
    for path in paths:
        if args.names and path.parent.name not in args.names:
            continue
        try:
            for name, source in read_sources(path):
                try:
                    status, detail = check_source(source, args)
                    report(status, name, detail)
                except (OSError, ValueError, KeyError, TypeError, URLError, subprocess.SubprocessError) as error:
                    report("ERROR", name, str(error))
        except (OSError, ValueError, yaml.YAMLError) as error:
            report("ERROR", path.parent.name, str(error))
    print("Summary: " + ", ".join(f"{status}={counts[status]}" for status in ("UPDATE", "OK", "SKIP", "ERROR")))
    return 1 if counts["ERROR"] else 0


if __name__ == "__main__":
    sys.exit(main())
