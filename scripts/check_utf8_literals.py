#!/usr/bin/env python3
"""Fail if a non-ASCII C++ string literal reaches juce::String unsafely.

WHY THIS EXISTS
juce::String (const char*) does NOT read UTF-8. It treats the bytes as 8-bit
(and asserts in Debug), so a Japanese literal handed to it directly becomes
mojibake on screen: each UTF-8 byte shown as a separate Latin-1 character.
That happened more than once in this codebase, and nothing caught it because
the build succeeds and the text only looks wrong at run time.

WHAT IT CHECKS
Every string literal that contains a non-ASCII byte must sit, directly, inside
one of the calls that decode UTF-8 properly (see SAFE). Anything else fails
the check with file:line and the call it was found in.

A literal counts as safe when the innermost call around it is one of SAFE.
Adjacent literals ("a" "b") belong to the same call, which is how the long
bilingual tooltips are written.

Every name in SAFE has been READ and decodes all of its string arguments with
juce::String::fromUTF8. Do not add a name here without doing the same: the
whole point is that the list cannot silently vouch for something that is not
actually safe. (initHead was only half safe -- it decoded its Japanese
argument but not its English one -- and was fixed before being listed.)

The one legitimate exception is a literal stored in a table and decoded where
it is used. Mark that line with a trailing `// utf8-ok: <where it is decoded>`
so the exception is explicit and reviewable rather than invisible.

It also fails if any source file is not valid UTF-8 at all, which is the other
way the same bug arrives: a file saved in Shift-JIS decodes to garbage however
carefully every literal is wrapped.

Comments are ignored, as are char literals, which this codebase does not use
for UI text.
"""
import pathlib, re, sys

# Calls audited to decode every string argument as UTF-8. See the docstring
# before adding to this list.
SAFE = {
    "fromUTF8",          # juce::String::fromUTF8
    "CharPointer_UTF8",  # juce::CharPointer_UTF8
    "u8",
    "tip",               # VoxMorphEditor::tip   -> fromUTF8 (en), fromUTF8 (jp)
    "vmTip",             # vmTip                 -> fromUTF8 (en), fromUTF8 (jp)
    "tipOf",             # AsmrPanel::tipOf      -> vmTip
    "initHead",          # FxChainPanel initHead -> fromUTF8 (en), fromUTF8 (jp)
}

def literals_with_context(src):
    """Yield (line, text, enclosing_call) for every "..." literal."""
    i, n, line = 0, len(src), 1
    stack = []            # enclosing '(' positions -> the identifier before it
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1; i += 1; continue
        if src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j; continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            seg = src[i:(n if j < 0 else j + 2)]
            line += seg.count("\n"); i = n if j < 0 else j + 2; continue
        if c == "'":                                   # char literal
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1; continue
        if c == "(":
            m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", src[max(0, i - 80):i])
            stack.append(m.group(1) if m else "")
            i += 1; continue
        if c == ")":
            if stack: stack.pop()
            i += 1; continue
        if c == '"':
            start_line = line
            j = i + 1; buf = []
            while j < n and src[j] != '"':
                if src[j] == "\\": buf.append(src[j:j+2]); j += 2; continue
                if src[j] == "\n": line += 1
                buf.append(src[j]); j += 1
            yield start_line, "".join(buf), (stack[-1] if stack else "")
            i = j + 1; continue
        i += 1

def main(roots):
    bad = []
    files = []
    for r in roots:
        p = pathlib.Path(r)
        files += sorted(p.rglob("*.h")) + sorted(p.rglob("*.cpp")) if p.is_dir() else [p]
    for f in files:
        try:
            src = f.read_bytes().decode("utf-8")
        except UnicodeDecodeError as e:
            bad.append(f"{f}: not valid UTF-8 ({e}); save it as UTF-8")
            continue
        exempt = {i + 1 for i, l in enumerate(src.split("\n")) if "utf8-ok:" in l}
        for ln, text, call in literals_with_context(src):
            if ln in exempt:
                continue
            if any(ord(ch) > 127 for ch in text) and call not in SAFE:
                bad.append(f"{f}:{ln}: non-ASCII literal inside '{call or '<no call>'}' "
                           f"(wrap it in juce::String::fromUTF8): {text[:40]!r}")
    for b in bad: print(b)
    print(f"\n{len(bad)} unsafe non-ASCII literal(s) in {len(files)} file(s)")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["src"]))
