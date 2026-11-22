#!/usr/bin/env python3
"""scripts/check-doc-links.py

Checks that every link into the docs resolves, and exits 1 naming each one that does not. The docs
keep each fact in one home and point at it by heading anchor (AGENTS.md, "Rules for the docs"), so
a renamed heading or a deleted page breaks links elsewhere without anyone seeing it.

It checks, in the git-tracked and untracked-but-not-ignored files of the checkout it is in:

- Markdown links `](path)` and `](path#anchor)` in docs/, .claude/skills/, AGENTS.md and
  CLAUDE.md, relative to the file they are in: the file exists, and the anchor is a heading of it.
- Citations written as plain text, `docs/<page>.md` and `docs/<page>.md#<anchor>`, in those same
  files and in src/, test/, cmake/, scripts/, docker/, web/src/, .github/, CMakeLists.txt and the
  Makefile: comments cite the docs this way.

An anchor is a heading's GitHub slug: lower case, a link's text kept and its target dropped,
backticks dropped, every character that is not a letter, digit, space, hyphen or underscore
dropped, spaces turned into hyphens; a second heading of the same slug gets -1, and so on. Fenced
code blocks hold no headings and no links.

Run it as `make docs-check` (CI's format job does), or directly. It needs only Python 3 and git.
"""
import os
import re
import subprocess
import sys

MARKDOWN = ["docs", ".claude/skills", "AGENTS.md", "CLAUDE.md"]
CODE = ["src", "test", "cmake", "scripts", "docker", "web/src", ".github", "CMakeLists.txt", "Makefile"]

LINK = re.compile(r"\]\(([^)\s]+)\)")
SCHEME = re.compile(r"[a-z][a-z0-9+.-]*:", re.I)
CITATION = re.compile(r"(docs/[\w\-/]+\.md)(#[\w\-]+)?")
# In Markdown a citation is plain text, never the tail of a longer path or a link's target (the
# link check has those, relative to their file).
PROSE_CITATION = re.compile(r"(?<![\w/(])(docs/[\w\-/]+\.md)(#[\w\-]+)?")
FENCE = re.compile(r"```.*?```", re.S)


def files(paths):
    listed = subprocess.check_output(["git", "ls-files", "-co", "--exclude-standard", "--", *paths], text=True)
    return [name for name in listed.split("\n") if name and os.path.isfile(name)]


def slug(heading):
    heading = heading.strip().lower()
    heading = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    heading = heading.replace("`", "")
    heading = re.sub(r"[^\w\- ]", "", heading)
    return heading.replace(" ", "-")


_anchors = {}


def anchors(path):
    if path not in _anchors:
        found, seen, fenced = set(), {}, False
        with open(path, encoding="utf-8") as page:
            for line in page:
                if line.startswith("```"):
                    fenced = not fenced
                    continue
                heading = None if fenced else re.match(r"#{1,6}\s+(.*)", line)
                if heading:
                    name = slug(heading.group(1))
                    count = seen.get(name, 0)
                    seen[name] = count + 1
                    found.add(name if count == 0 else f"{name}-{count}")
        _anchors[path] = found
    return _anchors[path]


def why_broken(target, anchor):
    """None when `target` exists and holds `anchor` (if one is named)."""
    if not os.path.exists(target):
        return "no such file"
    if anchor and target.endswith(".md") and anchor not in anchors(target):
        return "no such heading"
    return None


def main():
    os.chdir(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())
    broken, checked = [], 0

    for name in files(MARKDOWN):
        if not name.endswith(".md"):
            continue
        with open(name, encoding="utf-8") as page:
            text = FENCE.sub("", page.read())
        for link in LINK.finditer(text):
            target = link.group(1)
            if SCHEME.match(target):
                continue
            path, _, anchor = target.partition("#")
            resolved = os.path.normpath(os.path.join(os.path.dirname(name), path)) if path else name
            checked += 1
            if reason := why_broken(resolved, anchor):
                broken.append((name, target, reason))
        for cite in PROSE_CITATION.finditer(text):
            checked += 1
            if reason := why_broken(cite.group(1), (cite.group(2) or "")[1:]):
                broken.append((name, cite.group(0), reason))

    for name in files(CODE):
        try:
            with open(name, encoding="utf-8") as source:
                text = source.read()
        except UnicodeDecodeError:
            continue
        for cite in CITATION.finditer(text):
            checked += 1
            if reason := why_broken(cite.group(1), (cite.group(2) or "")[1:]):
                broken.append((name, cite.group(0), reason))

    for name, target, reason in broken:
        print(f"{name}: {target}: {reason}", file=sys.stderr)
    print(f"{checked} links and citations into the docs checked, {len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
