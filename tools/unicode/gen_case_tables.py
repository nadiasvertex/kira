#!/usr/bin/env python3
"""Generates src/std/unicode_tables.cn from the Unicode Character Database.

Offline, one-time generation tool -- not part of the Bazel build. Regenerate
when the UCD updates:

    curl -O https://www.unicode.org/Public/UCD/latest/ucd/UnicodeData.txt
    curl -O https://www.unicode.org/Public/UCD/latest/ucd/CaseFolding.txt
    curl -O https://www.unicode.org/Public/UCD/latest/ucd/SpecialCasing.txt
    python3 tools/unicode/gen_case_tables.py \
        UnicodeData.txt CaseFolding.txt SpecialCasing.txt \
        src/std/unicode_tables.cn

Emits, as parallel-array `static` globals consumed by src/std/unicode.cn:
  - SIMPLE_UPPER_KEY/VAL, SIMPLE_LOWER_KEY/VAL: 1:1 simple case mappings
    (every code point whose simple upper/lower differs from itself), sorted
    by key for binary search.
  - FOLD_KEY/CP0/CP1/CP2/CP3/COUNT: full case folding (status C + F from
    CaseFolding.txt), up to 4 result code points per entry.
  - UPPER_FULL_*/LOWER_FULL_*: same 4-slot shape, for the *default* (locale-
    independent, unconditional) multi-code-point full mappings in
    SpecialCasing.txt. Locale-tailored entries (tr/az/lt) are excluded --
    real, deliberately out of scope; see 52-std-string.md's Limitations.
    The one non-locale conditional entry, Final_Sigma (0x03A3), is excluded
    from this table and handled directly in unicode.cn, since it depends
    on surrounding context rather than the code point alone.
  - CASED_LO/HI, CASE_IGNORABLE_LO/HI: sorted, non-overlapping code point
    ranges used only to evaluate the Final_Sigma condition. A code point is
    "cased" here if its General_Category is Lu/Ll/Lt or it has any simple
    case mapping; "case-ignorable" is approximated as General_Category
    Mn/Me/Cf (the dominant real-world case, not the full derived property).
"""

import sys


def parse_unicode_data(path):
    """Returns (simple_upper, simple_lower, cased_ranges, ignorable_ranges).

    simple_upper/simple_lower: dict[int, int], only entries that differ from
    the code point itself.
    cased_ranges/ignorable_ranges: sorted lists of (lo, hi) inclusive ranges.
    """
    simple_upper = {}
    simple_lower = {}
    cased_points = []
    ignorable_points = []

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            fields = line.split(";")
            name = fields[1]
            if name.endswith("First>") or name.endswith("Last>"):
                # Large contiguous blocks (CJK, etc.) -- verified separately
                # that none carry case mappings; General_Category for these
                # is never Lu/Ll/Lt, so skipping them is exact, not lossy.
                continue
            cp = int(fields[0], 16)
            category = fields[2]
            upper = fields[12]
            lower = fields[13]

            if category in ("Lu", "Ll", "Lt"):
                cased_points.append(cp)
            elif category in ("Mn", "Me", "Cf"):
                ignorable_points.append(cp)

            if upper:
                u = int(upper, 16)
                if u != cp:
                    simple_upper[cp] = u
                    cased_points.append(cp)
            if lower:
                l = int(lower, 16)
                if l != cp:
                    simple_lower[cp] = l
                    cased_points.append(cp)

    return (simple_upper, simple_lower,
            to_ranges(sorted(set(cased_points))),
            to_ranges(sorted(set(ignorable_points))))


def to_ranges(sorted_points):
    if not sorted_points:
        return []
    ranges = []
    lo = hi = sorted_points[0]
    for cp in sorted_points[1:]:
        if cp == hi + 1:
            hi = cp
        else:
            ranges.append((lo, hi))
            lo = hi = cp
    ranges.append((lo, hi))
    return ranges


def parse_case_folding(path):
    """Returns dict[int, list[int]] for status C and F entries."""
    fold = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [x.strip() for x in line.split(";")]
            if len(fields) < 3:
                continue
            cp_hex, status, mapping = fields[0], fields[1], fields[2]
            if status not in ("C", "F"):
                continue  # skip S (simple-only alt) and T (Turkic-only)
            cp = int(cp_hex, 16)
            mapped = [int(x, 16) for x in mapping.split()]
            fold[cp] = mapped
    return fold


def parse_special_casing(path):
    """Returns (upper_full, lower_full) dict[int, list[int]].

    Only default (locale-independent) entries. Multi-code-point mappings
    only -- 1:1 entries duplicate UnicodeData.txt's simple mapping and add
    nothing. Final_Sigma (0x03A3) is excluded; handled contextually.
    """
    upper_full = {}
    lower_full = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [x.strip() for x in line.split(";")]
            # code; lower; title; upper; [conditions]
            if len(fields) < 4:
                continue
            cp = int(fields[0], 16)
            lower = [int(x, 16) for x in fields[1].split()] if fields[1] else []
            upper = [int(x, 16) for x in fields[3].split()] if fields[3] else []
            conditions = fields[4] if len(fields) > 4 else ""
            if conditions:
                # Any condition present at all marks a locale-tailored or
                # context-dependent (Final_Sigma) row -- both out of scope
                # here (Final_Sigma is handled directly in unicode.cn).
                continue
            if len(lower) > 1:
                lower_full[cp] = lower
            if len(upper) > 1:
                upper_full[cp] = upper
    return upper_full, lower_full


def emit_kv_table(out, name, mapping):
    # `array[T, N]` has no `.len()` (only `slice`/`list`/`str` do; an
    # array's length is already a compile-time constant on its type) -- a
    # `_LEN` constant lets unicode.cn form `&NAME_KEY[0..NAME_LEN]`
    # directly instead.
    keys = sorted(mapping.keys())
    out.write(f"static {name}_LEN: usize = {len(keys)}\n\n")
    out.write(f"static {name}_KEY: array[uint32, {len(keys)}] = [\n    ")
    out.write(", ".join(f"0x{k:X}" for k in keys))
    out.write("\n]\n\n")
    out.write(f"static {name}_VAL: array[uint32, {len(keys)}] = [\n    ")
    out.write(", ".join(f"0x{mapping[k]:X}" for k in keys))
    out.write("\n]\n\n")


def emit_multi_table_4(out, name, mapping):
    # Every multi-code-point table uses the same 4-slot shape (the widest
    # real mapping, FOLD's 3-code-point entries plus one spare, needs no
    # more) so src/std/unicode.cn has one lookup routine for all of them.
    keys = sorted(mapping.keys())
    for k in keys:
        assert len(mapping[k]) <= 4, (k, mapping[k])
    out.write(f"static {name}_LEN: usize = {len(keys)}\n\n")
    out.write(f"static {name}_KEY: array[uint32, {len(keys)}] = [\n    ")
    out.write(", ".join(f"0x{k:X}" for k in keys))
    out.write("\n]\n\n")
    out.write(f"static {name}_COUNT: array[uint8, {len(keys)}] = [\n    ")
    out.write(", ".join(str(len(mapping[k])) for k in keys))
    out.write("\n]\n\n")
    for i in range(4):
        out.write(f"static {name}_CP{i}: array[uint32, {len(keys)}] = [\n    ")
        out.write(", ".join(
            f"0x{(mapping[k][i] if i < len(mapping[k]) else 0):X}" for k in keys))
        out.write("\n]\n\n")


def emit_range_table(out, name, ranges):
    out.write(f"static {name}_LEN: usize = {len(ranges)}\n\n")
    out.write(f"static {name}_LO: array[uint32, {len(ranges)}] = [\n    ")
    out.write(", ".join(f"0x{lo:X}" for lo, _ in ranges))
    out.write("\n]\n\n")
    out.write(f"static {name}_HI: array[uint32, {len(ranges)}] = [\n    ")
    out.write(", ".join(f"0x{hi:X}" for _, hi in ranges))
    out.write("\n]\n\n")


def main():
    if len(sys.argv) != 5:
        print(f"usage: {sys.argv[0]} UnicodeData.txt CaseFolding.txt "
              "SpecialCasing.txt OUT.cn", file=sys.stderr)
        return 1
    unicode_data_path, case_folding_path, special_casing_path, out_path = sys.argv[1:]

    simple_upper, simple_lower, cased_ranges, ignorable_ranges = \
        parse_unicode_data(unicode_data_path)
    fold = parse_case_folding(case_folding_path)
    upper_full, lower_full = parse_special_casing(special_casing_path)

    with open(out_path, "w", encoding="utf-8") as out:
        out.write("module std.unicode_tables\n\n")
        out.write("# GENERATED FILE -- do not hand-edit.\n")
        out.write("# Produced by tools/unicode/gen_case_tables.py from the\n")
        out.write("# Unicode Character Database (UnicodeData.txt, CaseFolding.txt,\n")
        out.write("# SpecialCasing.txt). See that script's module doc comment for\n")
        out.write("# the exact table shapes and the regeneration command.\n")
        out.write("#\n")
        out.write("# Every KEY array is sorted ascending -- src/std/unicode.cn's\n")
        out.write("# lookups depend on this for binary search.\n\n")

        out.write("# --- Simple (1:1) case mapping ------------------------------\n\n")
        emit_kv_table(out, "SIMPLE_UPPER", simple_upper)
        emit_kv_table(out, "SIMPLE_LOWER", simple_lower)

        out.write("# --- Full case folding (status C + F) -----------------------\n\n")
        emit_multi_table_4(out, "FOLD", fold)

        out.write("# --- Full (multi-code-point) casing, default/unconditional --\n\n")
        emit_multi_table_4(out, "UPPER_FULL", upper_full)
        emit_multi_table_4(out, "LOWER_FULL", lower_full)

        out.write("# --- Final_Sigma support: cased / case-ignorable ranges -----\n\n")
        emit_range_table(out, "CASED", cased_ranges)
        emit_range_table(out, "CASE_IGNORABLE", ignorable_ranges)

    print(f"wrote {out_path}: "
          f"{len(simple_upper)} upper, {len(simple_lower)} lower, "
          f"{len(fold)} fold, {len(upper_full)} upper_full, "
          f"{len(lower_full)} lower_full, {len(cased_ranges)} cased ranges, "
          f"{len(ignorable_ranges)} ignorable ranges", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
