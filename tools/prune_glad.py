#!/usr/bin/env python3
"""Drops the GL functions the program never calls from a glad 0.1.x generated glad.c: their function pointer
variables, their names and their load statements (together about 16 KB of the binary). glad.h is not touched.

usage: prune_glad.py glad.c source.c [source.c ...] > glad_pruned.c

A function counts as used when it appears as an identifier (glFoo) in one of the given sources, or when glad.c
itself calls it (glGetString and so on). Run it again after calling a new GL function: a function that was
pruned shows up as 'undefined reference to glad_glFoo' at link time, never as a wrong call.

Pruning cannot be undone: keep the complete glad.c that glad generated (GL 4.2 core, GL_ARB_buffer_storage) somewhere,
for example in git history or as glad_full.c, and always prune that one."""
import re, sys

def main():
    glad_path, srcs = sys.argv[1], sys.argv[2:]
    lines = open(glad_path).read().split('\n')
    pat_def = re.compile(r'^PFNGL\w+PROC\s+glad_(gl\w+)\s*=\s*NULL;')
    pat_load = re.compile(r'^\s*glad_(gl\w+)\s*=\s*\(PFNGL\w+PROC\)\s*load\("(gl\w+)"\);')
    used = set()
    for s in srcs:
        used |= set(re.findall(r'\b(gl[A-Z]\w*)\b', open(s).read()))
    # what glad.c calls itself (glGetString, glGetStringi, glGetIntegerv)
    for l in lines:
        if pat_def.match(l) or pat_load.match(l):
            continue
        used |= set(re.findall(r'\b(gl[A-Z]\w*)\b', l))
    out, dropped = [], 0
    for l in lines:
        m = pat_def.match(l) or pat_load.match(l)
        if m and m.group(1) not in used:
            dropped += 1
            continue
        out.append(l)
    sys.stdout.write('\n'.join(out))
    sys.stderr.write('kept %d functions, dropped %d definitions and loads\n' % (len(used & set(re.findall(r'glad_(gl\w+)\s*=\s*NULL', '\n'.join(lines)))), dropped))

main()
