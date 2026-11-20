#!/usr/bin/env python3
"""Generate the Notepad++ UDL 2.1 definition for YANG (RFC 6020 / RFC 7950).

Both variants are named "YANG" (only one is installed at a time); they differ
only in foreground colours. Backgrounds follow the active theme."""
from xml.sax.saxutils import escape

KW = {
    # 1 - module header / linkage
    1: "module submodule import include prefix namespace belongs-to revision "
       "revision-date yang-version",
    # 2 - data definition / schema tree
    2: "container leaf leaf-list list choice case anydata anyxml grouping uses "
       "augment refine rpc action notification input output",
    # 3 - typing / identities / features / deviations
    3: "typedef type identity base feature if-feature extension argument "
       "deviation deviate",
    # 4 - properties & constraints
    4: "config default key mandatory min-elements max-elements ordered-by presence "
       "status unique units when must range length pattern modifier enum bit value "
       "position fraction-digits path require-instance error-message error-app-tag "
       "yin-element",
    # 5 - built-in types
    5: "binary bits boolean decimal64 empty enumeration identityref "
       "instance-identifier int8 int16 int32 int64 leafref string uint8 uint16 "
       "uint32 uint64 union",
    # 6 - literal argument values
    6: "true false current system user unbounded min max not-supported add delete "
       "replace invert-match and or not",
    # 7 - documentation statements (muted so long text recedes)
    7: "description reference contact organization",
    # 8 - status values worth noticing
    8: "deprecated obsolete",
}

# style name -> (light fg, dark fg, fontStyle)  fontStyle: 1=bold 2=italic 4=underline
STYLES = {
    "COMMENTS":          ("008000", "6A9955", 2),
    "LINE COMMENTS":     ("008000", "6A9955", 2),
    "NUMBERS":           ("FF6600", "B5CEA8", 0),
    "KEYWORDS1":         ("8000FF", "C586C0", 1),
    "KEYWORDS2":         ("0000FF", "569CD6", 1),
    "KEYWORDS3":         ("008080", "4EC9B0", 1),
    "KEYWORDS4":         ("0080C0", "9CDCFE", 0),
    "KEYWORDS5":         ("A31515", "4FC1FF", 0),
    "KEYWORDS6":         ("FF6600", "D7BA7D", 0),
    "KEYWORDS7":         ("808080", "808080", 2),
    "KEYWORDS8":         ("FF0000", "F44747", 1),
    "OPERATORS":         ("000080", "D4D4D4", 1),
    "FOLDER IN CODE1":   ("000080", "FFD700", 1),
    "FOLDER IN CODE2":   ("000000", "D4D4D4", 0),
    "FOLDER IN COMMENT": ("008000", "6A9955", 0),
    "DELIMITERS1":       ("804000", "CE9178", 0),   # "double quoted"
    "DELIMITERS2":       ("804000", "CE9178", 0),   # 'single quoted'
}
for i in range(3, 9):
    STYLES[f"DELIMITERS{i}"] = ("000000", "D4D4D4", 0)

KEYWORD_LISTS = [
    ("Comments", "00// 01 02((EOL)) 03/* 04*/"),
    ("Numbers, prefix1", ""), ("Numbers, prefix2", ""),
    ("Numbers, extras1", ""), ("Numbers, extras2", ""),
    ("Numbers, suffix1", ""), ("Numbers, suffix2", ""),
    ("Numbers, range", ".."),
    ("Operators1", "; : + | ="),
    ("Operators2", ""),
    ("Folders in code1, open", "{"),
    ("Folders in code1, middle", ""),
    ("Folders in code1, close", "}"),
    ("Folders in code2, open", ""), ("Folders in code2, middle", ""),
    ("Folders in code2, close", ""),
    ("Folders in comment, open", ""), ("Folders in comment, middle", ""),
    ("Folders in comment, close", ""),
] + [(f"Keywords{i}", KW[i]) for i in range(1, 9)] + [
    # Delim1: "..." with \ escape (multi-line allowed); Delim2: '...' no escape
    ("Delimiters", '00" 01\\ 02" 03\' 04 05\' 06 07 08 09 10 11 12 13 14 15 '
                   '16 17 18 19 20 21 22 23'),
]


def build(name: str, dark: bool) -> str:
    out = ['<?xml version="1.0" encoding="UTF-8" ?>',
           "<NotepadPlus>",
           f'    <UserLang name="{name}" ext="yang" udlVersion="2.1">',
           "        <Settings>",
           '            <Global caseIgnored="no" allowFoldOfComments="yes" '
           'foldCompact="no" forcePureLC="0" decimalSeparator="0" />',
           '            <Prefix ' + " ".join(f'Keywords{i}="no"' for i in range(1, 9)) + " />",
           "        </Settings>",
           "        <KeywordLists>"]
    for k, v in KEYWORD_LISTS:
        out.append(f'            <Keywords name="{k}">{escape(v)}</Keywords>')
    out += ["        </KeywordLists>", "        <Styles>",
            # DEFAULT: colorStyle 0 -> fg & bg inherit from the active theme
            '            <WordsStyle name="DEFAULT" fgColor="000000" bgColor="FFFFFF" '
            'colorStyle="0" fontName="" fontStyle="0" nesting="0" />']
    for s, (lfg, dfg, fs) in STYLES.items():
        fg = dfg if dark else lfg
        # colorStyle 1 -> apply foreground only, background stays transparent (theme)
        out.append(f'            <WordsStyle name="{s}" fgColor="{fg}" bgColor="FFFFFF" '
                   f'colorStyle="1" fontName="" fontStyle="{fs}" nesting="0" />')
    out += ["        </Styles>", "    </UserLang>", "</NotepadPlus>", ""]
    return "\n".join(out)


if __name__ == "__main__":
    import os
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    light, dark = build("YANG", False), build("YANG", True)
    # standalone files for manual install
    for fname, body in [("YANG.xml", light), ("YANG_dark.xml", dark)]:
        with open(os.path.join(root, "udl", fname), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(body)
    # embedded copy for the plugin's "Install YANG syntax highlighting" command
    assert ')UDL"' not in light + dark
    with open(os.path.join(root, "src", "udl_xml.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tools/gen_udl.py - do not edit.\n#pragma once\n")
        f.write('static const char kUdlLight[] = R"UDL(' + light + ')UDL";\n')
        f.write('static const char kUdlDark[] = R"UDL(' + dark + ')UDL";\n')
    print("wrote udl/YANG.xml, udl/YANG_dark.xml, src/udl_xml.h")
