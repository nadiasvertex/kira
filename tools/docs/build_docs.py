#!/usr/bin/env python3
"""Builds the Cinder language specification (spec/specification/) into an
epub, a pdf, and a static multi-page website, via pandoc.

Usage:
    build_docs.py <epub|pdf|site|all> [output_dir]

`epub`/`pdf` concatenate every chapter into one pandoc document, in chapter
order. Cross-chapter markdown links (`[x](../foo/31-bar.md#section)`) do not
survive concatenation as-is, so each chapter's top heading is given an
explicit anchor equal to its filename stem, and every cross-chapter link is
rewritten to point at that anchor (dropping any sub-section fragment, since
sub-section headings repeat across chapters and pandoc's auto-generated ids
for them are not unique in a merged document).

`site` renders each chapter to its own standalone HTML file, mirroring
spec/specification/'s directory layout exactly, and generates an index.html.
Because the site preserves the source layout 1:1, cross-chapter links only
need `.md` -> `.html`; fragments are left untouched and remain correct.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC_ROOT = REPO_ROOT / "spec" / "specification"
TITLE = "The Cinder Language and Standard Library Specification"
AUTHOR = " Cinder Contributors"

LINK_RE = re.compile(r"(\[[^\]]*\]\()([^)\s]+)(\))")
H1_RE = re.compile(r"^(#\s+.*)$", re.MULTILINE)


def chapter_files() -> list[Path]:
    """Every chapter file under SPEC_ROOT, in reading order (numeric prefix)."""
    files = [p for p in SPEC_ROOT.rglob("*.md") if p.name != "STYLE.md"]

    def key(p: Path) -> int:
        m = re.match(r"(\d+)-", p.name)
        return int(m.group(1)) if m else -1

    return sorted(files, key=key)


def stem(p: Path) -> str:
    return p.stem


def run_pandoc(args: list[str]) -> None:
    subprocess.run(["pandoc", *args], check=True, cwd=REPO_ROOT)


def resolve_link_target(md_path: Path, target: str) -> Path | None:
    """Resolves a relative markdown link's path part to an absolute Path,
    or None if it isn't a relative path to another file (an anchor-only
    link, or an absolute URL)."""
    if not target or target.startswith("#") or "://" in target:
        return None
    path_part = target.split("#", 1)[0]
    if not path_part:
        return None
    resolved = (md_path.parent / path_part).resolve()
    return resolved


# ---------------------------------------------------------------------------
# epub / pdf: one merged document
# ---------------------------------------------------------------------------


def build_merged_markdown(files: list[Path], workdir: Path) -> Path:
    stems = {f.resolve() for f in files}

    def rewrite(md_path: Path, text: str) -> str:
        # Give the chapter's own top-level heading a stable, unique anchor.
        text, n = H1_RE.subn(
            lambda m: f"{m.group(1)} {{#{stem(md_path)}}}", text, count=1
        )

        def replace_link(m: re.Match) -> str:
            pre, target, post = m.group(1), m.group(2), m.group(3)
            resolved = resolve_link_target(md_path, target)
            if resolved is not None and resolved in stems:
                return f"{pre}#{resolved.stem}{post}"
            return m.group(0)

        return LINK_RE.sub(replace_link, text)

    out = workdir / "book.md"
    with out.open("w", encoding="utf-8") as fh:
        for f in files:
            fh.write(rewrite(f, f.read_text(encoding="utf-8")))
            fh.write("\n\n")
    return out


def build_epub(output: Path) -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        merged = build_merged_markdown(chapter_files(), Path(tmp))
        output.parent.mkdir(parents=True, exist_ok=True)
        run_pandoc(
            [
                str(merged),
                "-o",
                str(output),
                "--toc",
                "--toc-depth=2",
                "--metadata",
                f"title={TITLE}",
                "--metadata",
                f"author={AUTHOR}",
            ]
        )
    print(f"Wrote {output}")


def build_pdf(output: Path) -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        merged = build_merged_markdown(chapter_files(), Path(tmp))
        output.parent.mkdir(parents=True, exist_ok=True)
        run_pandoc(
            [
                str(merged),
                "-o",
                str(output),
                "--toc",
                "--toc-depth=2",
                "--pdf-engine=xelatex",
                "-V",
                "geometry:margin=1in",
                "-V",
                "monofont:Menlo",
                # The spec's prose uses math/logic symbols (⇒, ≠, ∈, ≥, ...)
                # Latin Modern (xelatex's default) doesn't cover; this does.
                "-V",
                "mainfont:Arial Unicode MS",
                "--metadata",
                f"title={TITLE}",
                "--metadata",
                f"author={AUTHOR}",
            ]
        )
    print(f"Wrote {output}")


# ---------------------------------------------------------------------------
# site: one HTML page per chapter, same directory layout as the source
# ---------------------------------------------------------------------------

CSS = """\
body { max-width: 46em; margin: 2em auto; padding: 0 1em;
       font-family: -apple-system, Helvetica, Arial, sans-serif; line-height: 1.5; }
pre, code { background: #f4f4f4; }
pre { padding: 0.75em; overflow-x: auto; }
nav.toc { margin-bottom: 2em; }
h1, h2, h3 { line-height: 1.25; }
a { color: #0645ad; }
"""


def build_site(output_dir: Path) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    (output_dir / "style.css").write_text(CSS, encoding="utf-8")

    files = chapter_files()

    def rewrite_links(md_path: Path, text: str) -> str:
        def replace_link(m: re.Match) -> str:
            pre, target, post = m.group(1), m.group(2), m.group(3)
            resolved = resolve_link_target(md_path, target)
            if resolved is not None and resolved.suffix == ".md":
                # Swap only the .md extension, keep any #fragment untouched.
                if "#" in target:
                    path_part, frag = target.split("#", 1)
                    new_target = f"{path_part[:-3]}.html#{frag}"
                else:
                    new_target = f"{target[:-3]}.html"
                return f"{pre}{new_target}{post}"
            return m.group(0)

        return LINK_RE.sub(replace_link, text)

    for f in files:
        rel = f.relative_to(SPEC_ROOT)
        dest = (output_dir / rel).with_suffix(".html")
        dest.parent.mkdir(parents=True, exist_ok=True)
        css_depth = len(rel.parts) - 1
        css_rel = "/".join([".."] * css_depth + ["style.css"])

        import tempfile

        with tempfile.NamedTemporaryFile(
            "w", suffix=".md", delete=False, dir=dest.parent, encoding="utf-8"
        ) as tmp_f:
            tmp_f.write(rewrite_links(f, f.read_text(encoding="utf-8")))
            tmp_path = Path(tmp_f.name)
        try:
            run_pandoc(
                [
                    str(tmp_path),
                    "-o",
                    str(dest),
                    "-s",
                    "--toc",
                    f"--css={css_rel}",
                    "--metadata",
                    f"title={f.stem}",
                ]
            )
        finally:
            tmp_path.unlink()

    write_site_index(output_dir, files)

    grammar = REPO_ROOT / "spec" / "cinder-grammar.ebnf"
    if grammar.exists():
        shutil.copy(grammar, output_dir / grammar.name)
    conventions = REPO_ROOT / "spec" / "CONVENTIONS.md"
    if conventions.exists():
        run_pandoc(
            [
                str(conventions),
                "-o",
                str(output_dir / "CONVENTIONS.html"),
                "-s",
                "--toc",
                "--css=style.css",
                "--metadata",
                "title=Conventions",
            ]
        )

    print(f"Wrote {output_dir}/index.html")


def write_site_index(output_dir: Path, files: list[Path]) -> None:
    sections: dict[str, list[Path]] = {}
    for f in files:
        rel = f.relative_to(SPEC_ROOT)
        section = str(rel.parent) if rel.parent != Path(".") else ""
        sections.setdefault(section, []).append(f)

    lines = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        f"<title>{TITLE}</title>",
        "<link rel='stylesheet' href='style.css'>",
        "</head><body>",
        f"<h1>{TITLE}</h1>",
    ]
    for section in sorted(sections):
        label = section if section else "Overview"
        lines.append(f"<h2>{label}</h2><ul>")
        for f in sections[section]:
            rel_html = f.relative_to(SPEC_ROOT).with_suffix(".html")
            title = extract_h1(f)
            lines.append(f"<li><a href='{rel_html.as_posix()}'>{title}</a></li>")
        lines.append("</ul>")
    lines.append("</body></html>")
    (output_dir / "index.html").write_text("\n".join(lines), encoding="utf-8")


def extract_h1(f: Path) -> str:
    m = H1_RE.search(f.read_text(encoding="utf-8"))
    if not m:
        return f.stem
    return re.sub(r"^#\s+", "", m.group(1)).strip()


# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] not in ("epub", "pdf", "site", "all"):
        print(__doc__)
        return 2

    mode = argv[1]
    out_root = Path(argv[2]) if len(argv) > 2 else REPO_ROOT / "dist" / "docs"

    if mode in ("epub", "all"):
        build_epub(out_root / "cinder-specification.epub")
    if mode in ("pdf", "all"):
        build_pdf(out_root / "cinder-specification.pdf")
    if mode in ("site", "all"):
        build_site(out_root / "site")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
