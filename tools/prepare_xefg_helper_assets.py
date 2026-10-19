"""Stage the pinned XeFG dependency inputs from a qualified extension archive.

Only the entries listed in the tracked manifest are read. GPU programs and
generated runtime witnesses remain external dependencies, not C++ source.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tempfile
import zipfile


def prepare(archive: Path, output: Path) -> None:
    manifest_path = Path(__file__).resolve().parents[1] / "src/FrameGen/XeFGExtension/AssetManifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if output.exists():
        raise ValueError(f"Output already exists; choose a new dependency directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source, tempfile.TemporaryDirectory(prefix="xefg-assets-", dir=output.parent) as temp:
        staged = Path(temp) / "bundle"
        staged.mkdir()
        for item in manifest["files"]:
            relative = PurePosixPath(item["path"])
            if relative.is_absolute() or ".." in relative.parts or ":" in item["path"] or "\\" in item["path"]:
                raise ValueError(f"Invalid manifest path: {item['path']}")
            entries = [entry for entry in source.infolist() if entry.filename == item["archivePath"]]
            if len(entries) != 1 or entries[0].file_size != item["size"]:
                raise ValueError(f"Missing, duplicate or incorrect-size archive input: {item['archivePath']}")
            data = source.read(entries[0])
            if hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError(f"Archive input hash mismatch: {item['archivePath']}")
            target = staged / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        record = {"schemaVersion": 1, "manifestSHA256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                  "files": manifest["files"]}
        (staged / "bundle-identity.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        staged.rename(output)
    print(f"Verified and staged {len(manifest['files'])} dependency files: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True, help="Qualified private-extension-source.zip")
    parser.add_argument("--output", type=Path, required=True, help="New dependency bundle directory")
    args = parser.parse_args()
    prepare(args.archive.resolve(), args.output.resolve())
