#!/usr/bin/env python3
"""Create a JSON update manifest from files supplied by the host project."""

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(64 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_relative_path(value):
    parts = value.split("/")
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or ":" in value
        or any(part in ("", ".", "..") for part in parts)
    ):
        raise ValueError("unsafe update path: {!r}".format(value))


def create_manifest(args):
    files = []
    seen_paths = set()
    for relative_path, download_url, source_path in args.file:
        validate_relative_path(relative_path)
        if relative_path in seen_paths:
            raise ValueError("duplicate update path: {!r}".format(relative_path))
        if not download_url:
            raise ValueError("download URL is empty for {!r}".format(relative_path))

        source = Path(source_path)
        if not source.is_file():
            raise ValueError("file not found: {}".format(source))

        files.append(
            {
                "path": relative_path,
                "download": download_url,
                "sha256": sha256(source),
            }
        )
        seen_paths.add(relative_path)

    if not seen_paths:
        raise ValueError("at least one --file entry is required")

    with args.output.open("w", encoding="utf-8") as output:
        json.dump(
            {
                "name": args.name,
                "platform": args.platform,
                "version": args.version,
                "build": args.build,
                "files": files,
            },
            output,
            indent=2,
        )
        output.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument(
        "--file",
        action="append",
        nargs=3,
        metavar=("RELATIVE_PATH", "DOWNLOAD_URL", "SOURCE_FILE"),
        required=True,
        help="manifest file entry; may be supplied more than once",
    )
    args = parser.parse_args()
    if args.build < 0:
        parser.error("--build must be non-negative")

    try:
        create_manifest(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
