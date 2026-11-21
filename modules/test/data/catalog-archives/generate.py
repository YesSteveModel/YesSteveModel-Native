"""Regenerate archive metadata fixtures; requires py7zr==1.1.4."""

from pathlib import Path
import random
import struct
import zipfile

import py7zr


def main():
    target = Path(__file__).resolve().parent
    payload = random.Random(42).randbytes(256 * 1024)
    with zipfile.ZipFile(target / "directory.zip", "w") as archive:
        archive.writestr("root/a.bin", payload)
        archive.writestr("root/b.txt", b"second")
    plain = (target / "directory.zip").read_bytes()
    end = plain[-22:]
    comment = b"comment-PK\x05\x06-invalid-header-" * 10
    (target / "directory-comment.zip").write_bytes(
        plain[:-2] + struct.pack("<H", len(comment)) + comment
    )
    (target / "directory-prefix.zip").write_bytes(b"MZ" + bytes(1022) + plain)
    directory_size, directory_offset = struct.unpack_from("<II", end, 12)
    position = len(plain) - 22
    zip64 = struct.pack(
        "<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 2, 2,
        directory_size, directory_offset,
    )
    locator = struct.pack("<IIQI", 0x07064B50, 0, position, 1)
    sentinel = struct.pack(
        "<IHHHHIIH", 0x06054B50, 0, 0, 65535, 65535,
        directory_size, 0xFFFFFFFF, 0,
    )
    (target / "directory-zip64.zip").write_bytes(plain[:-22] + zip64 + locator + sentinel)
    for encoded in (True, False):
        for crc in (True, False):
            name = ("encoded" if encoded else "plain") + ("" if crc else "-no-crc") + ".7z"
            with py7zr.SevenZipFile(target / name, "w", filters=[{"id": py7zr.FILTER_LZMA2, "preset": 1}]) as archive:
                archive.set_encoded_header_mode(encoded)
                archive.writestr(payload, "root/a.bin")
                archive.writestr(b"second", "root/b.txt")
                if not crc:
                    write_header = archive._write_header

                    def without_crc(archive=archive, write_header=write_header):
                        streams = archive.header.main_streams.substreamsinfo
                        streams.digestsdefined = [False] * len(streams.digestsdefined)
                        return write_header()

                    archive._write_header = without_crc


if __name__ == "__main__":
    main()
