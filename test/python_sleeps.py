"""python_sleeps.py <root> <glob>: the tests' Python sleeps only in a role (docs/testing.md#sleeps-by-role).

C++ test code cannot name a raw sleep at all (no_raw_sleeps.hpp poisons it); Python has no compiler to do that, so its
rule is read from each file's syntax tree, never its text: the name `sleep`, called, referred to or imported, from any
module, may appear only in the body of a module-level function named for a role (induce, negative, poll or pace), as
sendspin_server.py's are. Strings, comments and names that only contain the word (a library's sleep_to_limit_buffer)
are not the name. Every file under <root> matching <glob> is read; one that does not parse fails, and so does a check
of nothing.
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

ROLES = frozenset({"induce", "negative", "poll", "pace"})


def offences(tree: ast.Module) -> list[int]:
    lines: list[int] = []
    for statement in tree.body:
        if isinstance(statement, (ast.FunctionDef, ast.AsyncFunctionDef)) and statement.name in ROLES:
            continue
        for node in ast.walk(statement):
            named = (
                (isinstance(node, ast.Attribute) and node.attr == "sleep")
                or (isinstance(node, ast.Name) and node.id == "sleep")
                or (isinstance(node, ast.alias) and "sleep" in (node.name.split(".")[-1], node.asname))
            )
            if named:
                lines.append(node.lineno)
    return sorted(set(lines))


def main() -> int:
    root, pattern = Path(sys.argv[1]), sys.argv[2]
    files = sorted(path for path in root.rglob(pattern) if path.is_file())
    found = 0
    for path in files:
        shown = path.relative_to(root)
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError as error:
            print(f"{shown}: does not parse: {error}")
            found += 1
            continue
        for line in offences(tree):
            print(f"{shown}:{line}: a sleep outside a role (induce, negative, poll or pace)")
            found += 1
    if not files:
        print(f"python_sleeps: no file under {root} matches {pattern}: the check would check nothing")
        return 1
    if found:
        print(f"python_sleeps: {found} offences under {root} (docs/testing.md#sleeps-by-role)")
        return 1
    print(f"python_sleeps: {len(files)} files under {root}, every sleep in a role")
    return 0


if __name__ == "__main__":
    sys.exit(main())
