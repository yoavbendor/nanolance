"""Translate frostem's Snowball-generated Rust stemmer to C++ (labeled loops become for(;;) + goto)."""
import re, sys

src = open(sys.argv[1]).read()
out = []

# 1. among tables
def among_tables(text):
    res = []
    for m in re.finditer(r"static (A_\d+): &'static \[Among<Context>; (\d+)\] = &\[(.*?)\];", text, re.S):
        name, body = m.group(1), m.group(3)
        rows = re.findall(r'Among\("((?:[^"\\]|\\.)*)", (-?\d+), (-?\d+), None\)', body)
        assert len(rows) == int(m.group(2)), name
        res.append(f"const Among {name}[] = {{")
        for s, a, b in rows:
            res.append(f'    {{"{s}", {a}, {b}}},')
        res.append("};")
    for m in re.finditer(r"static (G_\w+): &'static \[u8; (\d+)\] = &\[([^\]]*)\];", text):
        res.append(f"const unsigned char {m.group(1)}[] = {{{m.group(3)}}};")
    return res

out += among_tables(src)

# 2. functions
body_start = src.index("fn r_") if "fn r_" in src else src.index("pub fn stem")
code = src[body_start:]
lines = code.split("\n")
stack = []  # per open brace: label kind or None
for raw in lines:
    line = raw.rstrip()
    s = line.strip()
    indent = line[: len(line) - len(line.lstrip())]
    if not s:
        continue
    # function headers
    m = re.match(r"(pub )?fn (\w+)\(env: &mut SnowballEnv(?:, context: &mut Context)?\) -> bool \{", s)
    if m:
        out.append(f"{indent}bool {m.group(2)}(SnowballEnv& env) {{")
        stack.append(None)
        continue
    if re.match(r"let mut context = &mut Context \{", s):
        stack.append("ctx")
        continue
    if s == "};" and stack and stack[-1] == "ctx":
        stack.pop()
        continue
    m = re.match(r"'(\w+): loop ?\{$", s)
    if m:
        out.append(f"{indent}for (;;) {{ {{")
        stack.append(("loop", m.group(1)))
        continue
    m = re.match(r"'(\w+): for _ in 0\.\.1 \{$", s)
    if m:
        out.append(f"{indent}for (int once_{m.group(1)} = 0; once_{m.group(1)} < 1; ++once_{m.group(1)}) {{ {{")
        stack.append(("loop", m.group(1)))
        continue
    m = re.match(r"match (\w+) \{$", s)
    if m:
        out.append(f"{indent}switch ({m.group(1)}) {{")
        stack.append("switch")
        continue
    m = re.match(r"(-?\d+) => \{$", s)
    if m:
        out.append(f"{indent}case {m.group(1)}: {{")
        stack.append("case")
        continue
    if s == "_ => ()":
        out.append(f"{indent}default: break;")
        continue
    if s.startswith("}"):
        kind = stack.pop()
        rest = s[1:]
        if isinstance(kind, tuple):
            out.append(f"{indent}}} {kind[1]}_cont:; }} {kind[1]}_end:;{rest}")
        elif kind == "case":
            out.append(f"{indent}}} break;{rest}")
        else:
            out.append(f"{indent}}}{rest}")
        continue
    # statements
    t = s
    t = re.sub(r"break '(\w+);", r"goto \1_end;", t)
    t = re.sub(r"continue '(\w+);", r"goto \1_cont;", t)
    t = re.sub(r"let mut (\w+) ?: ?bool;", r"bool \1 = false;", t)
    t = re.sub(r"let mut (\w+) ?: ?i32;", r"int \1 = 0;", t)
    t = re.sub(r"let mut among_var;", r"int among_var = 0;", t)
    t = re.sub(r"let (\w+) = ", r"int \1 = ", t)
    t = t.replace("env.current.as_bytes()[", "env.byte(")
    t = re.sub(r"env\.byte\(\(([^\]]*?)\) as usize\]", r"env.byte(\1)", t)
    t = re.sub(r" as (u8|i32|usize|u32)\b", "", t)
    t = t.replace('&"', '"')
    t = re.sub(r"find_among(_b)?\((A_\d+), context\)", r"find_among\1(\2, sizeof(\2) / sizeof(\2[0]))", t)
    t = re.sub(r"\(env, context\)", "(env)", t)
    m = re.match(r"(if|while) (.*) \{$", t)
    if m:
        cond = m.group(2)
        if not (cond.startswith("(") and cond.endswith(")") and cond.count("(") == cond.count(")")):
            cond = f"({cond})"
        t = f"{m.group(1)} {cond} {{"
        stack.append(None)
    elif t.endswith("{"):
        stack.append(None)
    if t == "return true":
        t = "return true;"
    out.append(indent + t)
print("\n".join(out))
