#!/usr/bin/env python3
"""Convert Doxygen XML for the vendored JUCE modules into an agent-friendly reference.

Writes into OUT_DIR:
  symbols.tsv          one line per class, member, free function, enum and macro
  classes/<name>.md    one page per class or struct (name = qualified name, '::' -> '.')
  modules.md           each module's description and its classes
  MANIFEST             JUCE version and the git tree id of juce/modules it was built from
"""

import argparse
import re
import subprocess
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

SOURCE_PREFIX = "juce/modules/"  # generate.sh strips Doxygen paths down to repo-relative ones


def source_path(doxygen_path):
    return doxygen_path


def module_of(doxygen_path):
    if not doxygen_path.startswith(SOURCE_PREFIX):
        return ""
    return doxygen_path[len(SOURCE_PREFIX):].split("/")[0]


def page_name(qualified):
    return qualified.replace("::", ".")


def one_line(text):
    return re.sub(r"\s+", " ", text or "").strip()


def plain(el):
    """Flatten an element to text, dropping markup (for types and signatures)."""
    return one_line("".join(el.itertext())) if el is not None else ""


# ---------------------------------------------------------------- description rendering

def render(el):
    """Render a Doxygen description element to Markdown."""
    if el is None:
        return ""
    out = []
    _render_children(el, out)
    text = "".join(out)
    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def _render_children(el, out):
    if el.text:
        out.append(el.text)
    for child in el:
        _render(child, out)
        if child.tail:
            out.append(child.tail)


def _render(el, out):
    tag = el.tag
    if tag == "para":
        out.append("\n\n")
        _render_children(el, out)
        out.append("\n\n")
    elif tag == "computeroutput":
        out.append("`" + "".join(el.itertext()).strip() + "`")
    elif tag in ("bold",):
        out.append("**" + "".join(el.itertext()) + "**")
    elif tag in ("emphasis",):
        out.append("*" + "".join(el.itertext()) + "*")
    elif tag == "programlisting":
        lines = ["".join(_code_text(cl)) for cl in el.findall("codeline")]
        out.append("\n\n```cpp\n" + "\n".join(lines).rstrip() + "\n```\n\n")
    elif tag == "verbatim":
        out.append("\n\n```\n" + (el.text or "").strip("\n") + "\n```\n\n")
    elif tag in ("itemizedlist", "orderedlist"):
        out.append("\n")
        for i, item in enumerate(el.findall("listitem"), 1):
            bullet = f"{i}." if tag == "orderedlist" else "-"
            out.append(f"\n{bullet} " + one_line(render(item)))
        out.append("\n\n")
    elif tag == "simplesect":
        kind = el.get("kind")
        label = {"return": "Returns", "see": "See also", "note": "Note", "warning": "Warning",
                 "pre": "Precondition", "post": "Postcondition", "attention": "Attention",
                 "remark": "Remark", "since": "Since", "deprecated": "Deprecated"}.get(kind, kind.title())
        out.append(f"\n\n**{label}:** " + render(el).strip() + "\n\n")
    elif tag == "parameterlist":
        out.append("\n\n**Parameters:**\n")
        for item in el.findall("parameteritem"):
            names = ", ".join(plain(n) for n in item.iter("parametername"))
            out.append(f"\n- `{names}`: " + one_line(render(item.find("parameterdescription"))))
        out.append("\n\n")
    elif tag == "xrefsect":
        title = plain(el.find("xreftitle"))
        out.append(f"\n\n**{title}:** " + render(el.find("xrefdescription")) + "\n\n")
    elif tag == "ulink":
        out.append(f"[{''.join(el.itertext())}]({el.get('url')})")
    elif tag in ("linebreak",):
        out.append("\n")
    elif tag in ("sp",):
        out.append(" ")
    elif tag in ("heading",):
        out.append("\n\n**" + "".join(el.itertext()) + "**\n\n")
    elif tag in ("internal",):
        pass  # implementation notes, not API
    else:  # ref, highlight, table cells and anything unknown: keep the text
        _render_children(el, out)


def _code_text(el):
    parts = []
    if el.text:
        parts.append(el.text)
    for child in el:
        parts.append(" " if child.tag == "sp" else "".join(_code_text(child)))
        if child.tail:
            parts.append(child.tail)
    return parts


def description(el):
    brief = render(el.find("briefdescription"))
    detail = render(el.find("detaileddescription"))
    return brief, detail


# ---------------------------------------------------------------- member signatures

def signature(member):
    kind = member.get("kind")
    name = plain(member.find("name"))
    if kind == "enum":
        values = [plain(v.find("name")) for v in member.findall("enumvalue")]
        strong = "enum class " if member.get("strong") == "yes" else "enum "
        return f"{strong}{name} {{ {', '.join(values)} }}"
    if kind == "define":
        params = [plain(p.find("defname")) for p in member.findall("param")]
        return f"#define {name}" + (f"({', '.join(params)})" if member.find("param") is not None else "")
    template = ""
    tpl = member.find("templateparamlist")
    if tpl is not None:
        template = "template <" + ", ".join(plain(p.find("type")) + (" " + plain(p.find("declname")) if p.find("declname") is not None else "") for p in tpl.findall("param")) + "> "
    prefix = []
    if member.get("static") == "yes":
        prefix.append("static")
    if member.get("virt") in ("virtual", "pure-virtual"):
        prefix.append("virtual")
    type_ = plain(member.find("type"))
    args = plain(member.find("argsstring"))
    if kind == "typedef":
        return one_line(plain(member.find("definition")))
    sig = " ".join(prefix + ([type_] if type_ else []) + [name]) + args
    init = plain(member.find("initializer"))
    if kind == "variable" and init:
        sig += " " + init
    return template + one_line(sig)


# ---------------------------------------------------------------- main conversion

def load(path):
    return ET.parse(path).getroot().find("compounddef")


def module_descriptions(repo):
    result = {}
    for header in sorted((repo / "juce/modules").glob("*/juce_*.h")):
        if header.stem != header.parent.name:
            continue
        match = re.search(r"BEGIN_JUCE_MODULE_DECLARATION(.*?)END_JUCE_MODULE_DECLARATION",
                          header.read_text(errors="replace"), re.S)
        if match:
            desc = re.search(r"description:\s*(.*)", match.group(1))
            deps = re.search(r"dependencies:\s*(.*)", match.group(1))
            result[header.stem] = (desc.group(1).strip() if desc else "",
                                   deps.group(1).strip() if deps else "")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("xml_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--repo", type=Path, required=True)
    args = parser.parse_args()

    index = ET.parse(args.xml_dir / "index.xml").getroot()
    compounds = [(c.get("refid"), c.get("kind")) for c in index.findall("compound")]

    classes_dir = args.out_dir / "classes"
    classes_dir.mkdir(parents=True, exist_ok=True)

    symbols = []  # (qualified, kind, signature, location, module, brief)
    by_module = defaultdict(list)

    for refid, kind in compounds:
        if kind not in ("class", "struct", "union", "namespace", "file"):
            continue
        cdef = load(args.xml_dir / f"{refid}.xml")
        if cdef is None or cdef.get("prot") == "private":
            continue
        qualified = plain(cdef.find("compoundname"))
        loc = cdef.find("location")
        dfile = loc.get("file") if loc is not None else ""
        module = module_of(dfile)
        location = f"{source_path(dfile)}:{loc.get('line')}" if loc is not None and loc.get("line") else source_path(dfile)
        brief, detail = description(cdef)

        members = []
        for section in cdef.findall("sectiondef"):
            skind = section.get("kind")
            if skind.startswith("private") or skind == "friend":
                continue
            for m in section.findall("memberdef"):
                if m.get("prot") == "private":
                    continue
                mkind = m.get("kind")
                if kind == "file" and mkind != "define":
                    continue  # free functions are listed under their namespace
                mloc = m.find("location")
                mfile = mloc.get("file") if mloc is not None else dfile
                mlocation = f"{source_path(mfile)}:{mloc.get('line')}" if mloc is not None else location
                mqualified = plain(m.find("qualifiedname")) or plain(m.find("name"))
                mbrief, mdetail = description(m)
                sig = signature(m)
                members.append((skind, sig, mlocation, mbrief, mdetail, m))
                symbols.append((mqualified, mkind, sig, mlocation, module_of(mfile), one_line(mbrief)))

        if kind in ("namespace", "file"):
            continue

        symbols.append((qualified, kind, f"{kind} {qualified}", location, module, one_line(brief)))
        by_module[module].append((qualified, one_line(brief)))

        lines = [f"# {qualified}", ""]
        meta = [f"- **Kind:** {kind}", f"- **Module:** {module}", f"- **Declared at:** `{location}`"]
        bases = [f"{b.get('prot')} {plain(b)}" for b in cdef.findall("basecompoundref")]
        if bases:
            meta.append("- **Inherits:** " + ", ".join(f"`{b}`" for b in bases))
        derived = [plain(d) for d in cdef.findall("derivedcompoundref")]
        if derived:
            meta.append("- **Derived classes:** " + ", ".join(f"`{d}`" for d in derived))
        inner = [plain(i) for i in cdef.findall("innerclass") if i.get("prot") != "private"]
        if inner:
            meta.append("- **Nested types:** " + ", ".join(f"`{i}`" for i in inner))
        lines += meta + [""]
        if brief:
            lines += [brief, ""]
        if detail:
            lines += [detail, ""]

        section_titles = {
            "public-type": "Public types", "public-func": "Public member functions",
            "public-static-func": "Public static functions", "public-attrib": "Public data members",
            "public-static-attrib": "Public static data", "protected-type": "Protected types",
            "protected-func": "Protected member functions", "protected-static-func": "Protected static functions",
            "protected-attrib": "Protected data members", "related": "Related functions",
            "user-defined": "Members",
        }
        grouped = defaultdict(list)
        for entry in members:
            grouped[entry[0]].append(entry)
        for skind in section_titles:
            if skind not in grouped:
                continue
            lines += [f"## {section_titles[skind]}", ""]
            for _, sig, mlocation, mbrief, mdetail, m in grouped[skind]:
                lines += [f"### `{sig}`", "", f"`{mlocation}`", ""]
                if mbrief:
                    lines += [mbrief, ""]
                if mdetail:
                    lines += [mdetail, ""]
                reimpl = [plain(r) for r in m.findall("reimplements")]
                if reimpl:
                    lines += ["Overrides `" + "`, `".join(reimpl) + "`.", ""]
                if m.get("kind") == "enum":
                    for v in m.findall("enumvalue"):
                        vbrief = one_line(render(v.find("briefdescription")) + " " + render(v.find("detaileddescription")))
                        lines.append(f"- `{plain(v.find('name'))}`" + (f": {vbrief}" if vbrief else ""))
                    lines.append("")
        (classes_dir / f"{page_name(qualified)}.md").write_text("\n".join(lines).rstrip() + "\n")

    symbols.sort(key=lambda s: (s[0].lower(), s[1], s[3]))
    with open(args.out_dir / "symbols.tsv", "w") as f:
        f.write("name\tkind\tsignature\tlocation\tmodule\tbrief\n")
        for row in symbols:
            f.write("\t".join(field.replace("\t", " ") for field in row) + "\n")

    descs = module_descriptions(args.repo)
    lines = ["# JUCE modules", ""]
    for module in sorted(set(descs) | set(by_module)):
        desc, deps = descs.get(module, ("", ""))
        lines += [f"## {module}", ""]
        if desc:
            lines += [desc, ""]
        if deps:
            lines += [f"Depends on: {deps}", ""]
        for qualified, brief in sorted(by_module.get(module, [])):
            lines.append(f"- `{qualified}`" + (f": {brief}" if brief else ""))
        lines.append("")
    (args.out_dir / "modules.md").write_text("\n".join(lines))

    header = (args.repo / "juce/modules/juce_core/system/juce_StandardHeader.h").read_text()
    version = ".".join(re.search(rf"#define\s+JUCE_{p}\s+(\d+)", header).group(1)
                       for p in ("MAJOR_VERSION", "MINOR_VERSION", "BUILDNUMBER"))
    tree = subprocess.run(["git", "-C", str(args.repo), "rev-parse", "HEAD:juce/modules"],
                          capture_output=True, text=True, check=True).stdout.strip()
    (args.out_dir / "MANIFEST").write_text(f"juce_version={version}\njuce_modules_tree={tree}\n")


if __name__ == "__main__":
    main()
