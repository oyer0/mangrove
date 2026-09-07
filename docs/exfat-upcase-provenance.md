# exFAT recommended up-case table provenance

`kernel/src/storage/exfat_upcase.inc` is generated from the recommended
compressed up-case table in the Microsoft exFAT File System Specification,
section 7.2.5.1, Table 25:

<https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification#7251-recommended-up-case-table>

The specification table is represented as UTF-16 words in
`tools/exfat_upcase_table.txt`. The project-owned generator converts those
words to the little-endian byte include used by the kernel. It does not use
Linux, exfatprogs, FUSE, or a Unicode database as input.

Regenerate and verify it with:

```text
python3 tools/generate_exfat_upcase.py
python3 tools/generate_exfat_upcase.py --check
```

The expected output is exactly 5,836 bytes with the exFAT up-case table
checksum `0xE619D30D`. The generated file must not be edited manually.
