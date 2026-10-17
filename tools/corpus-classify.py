#!/usr/bin/env python3
"""Classify each hand-written cache-line annotation by what it separates.

The census counted the sites. This asks the question that decides which
transformation is worth building: when a person fights false sharing by
hand, are they pushing two *fields* of one object apart, or pushing two
*objects* apart?

The rule is one line: the annotation separates FIELDS exactly when the thing
it annotates lives inside a record, and OBJECTS otherwise.

  field   a member inside a record body -- `spinlock_t lock ____cacheline_
          aligned_in_smp;` -- and also a nested anonymous record used to
          group members, which is the kernel's idiom:

              struct kioctx {
                  ...
                  struct { atomic_t reqs_available; } ____cacheline_aligned_in_smp;
                  struct { spinlock_t ctx_lock; ... } ____cacheline_aligned_in_smp;
              };

          That is field separation performed by hand, and reading the line
          rather than the nesting called it an object -- the first version
          of this script did, and understated the field share.

  object  a whole record or variable at file scope -- `struct
          __rte_cache_aligned q {`, `alignas(64) uint32_t table[]`.
          Reordering cannot help; only the allocation site can, and
          `alignas` cannot express it across two separate allocations.

  macro   `#define CACHE_LINE ...` and its guards: infrastructure, not a
          decision about two threads.

Nesting is read from the file rather than guessed from the line, because the
same spelling means both things: `} ____cacheline_aligned;` closes a record
(object) while `u8 buf[N] ____cacheline_aligned;` declares a member (field).
"""
import csv, re, sys
from collections import Counter, defaultdict
from pathlib import Path

CLOSES_RECORD = re.compile(r'^\s*\}')
OPENS_RECORD = re.compile(r'\b(?:struct|class|union)\b[^;]*\{')


def depth_map(path):
    """For each 1-based line, how many record bodies enclose its start.

    Four things the first version got wrong, each silently moving sites
    from `field` to `object`:

      - a record whose `{` sits on its own line (Allman) was never opened,
        so every member of it counted as an object. postgres reversed
        completely on this, PGPROC included -- the struct whose own comment
        says the alignment is "to avoid false sharing";
      - block comments spanning lines, and braces inside string and
        character literals, moved the counter;
      - `#if`/`#else` branches with unbalanced braces desynchronised it for
        the rest of the file;
      - `void f(struct foo *p) {` opened a record that is not one.

    And a fifth, found by a later review: the `)` that ends a parameter
    list also ended `struct alignas(64) Name {`, so a record declared with
    an attribute between the keyword and its brace never opened, and every
    member of it counted as an object. MySQL's log0sys.h is written that
    way, and the "largest shift towards objects" the document reported for
    MySQL was this bug. A `)` ends the pending record only outside any
    parentheses opened since the keyword.
    """
    try:
        text = Path(path).read_text(errors='replace')
    except OSError:
        return None

    depths, stack, brace = [], [], 0
    pending = 0          # a record keyword seen, its `{` not yet
    parens = 0           # parentheses opened since that keyword
    idents = 0           # identifiers seen since that keyword, outside parentheses
    colon = False        # a base-class list since that keyword
    saved = []           # brace state at each open #if, restored at #else
    in_comment = False
    for line in text.splitlines():
        depths.append(len(stack))

        # Strip comments and literals before counting anything.
        out, i, n = [], 0, len(line)
        while i < n:
            if in_comment:
                if line.startswith('*/', i):
                    in_comment = False; i += 2
                else:
                    i += 1
                continue
            if line.startswith('/*', i):
                in_comment = True; i += 2; continue
            if line.startswith('//', i):
                break
            c = line[i]
            if c in '"\'':
                i += 1
                while i < n and line[i] != c:
                    i += 2 if line[i] == '\\' else 1
                i += 1
                continue
            out.append(c); i += 1
        s = ''.join(out)

        if s.lstrip().startswith('#'):      # no braces here we can trust
            # The branches of a conditional are alternatives: a record
            # opened under #ifdef and again under #else is one record, not
            # two, and counting both left the rest of the file one level
            # too deep.
            d = s.lstrip()[1:].strip().split(' ')[0] if s.lstrip()[1:].strip() else ''
            if d in ('if', 'ifdef', 'ifndef'):
                saved.append((brace, list(stack), pending))
            elif d in ('else', 'elif') and saved:
                brace, stack, pending = saved[-1][0], list(saved[-1][1]), saved[-1][2]
            elif d == 'endif' and saved:
                saved.pop()
            continue

        for m in re.finditer(r'[{}();=]|(?<!:):(?!:)|\b(?:struct|class|union)\b|\b[A-Za-z_]\w*\b', s):
            tok = m.group(0)
            if tok in ('struct', 'class', 'union'):
                # A declaration is followed by a name and then `{`, possibly
                # on a later line; a parameter or cast by `*`, `)` or `,`.
                rest = s[m.end():].lstrip()
                if not rest.startswith(('*', ')', ',', '>')):
                    pending += 1; parens = 0; idents = 0; colon = False
            elif tok == '(' and pending:
                parens += 1
            elif tok == ')' and pending:
                if parens:
                    parens -= 1
                # Parentheses right after the keyword, or after one name,
                # belong to an attribute: `struct alignas(64) S {`. After
                # two names they are a parameter list -- `struct foo
                # make(void) {` -- and the brace that follows is a function
                # body, unless a base-class list is open.
                if parens == 0 and idents >= 2 and not colon:
                    pending = 0
            elif tok == ':' and pending and not parens:
                colon = True
            elif tok == '=' and pending and not parens:
                pending = 0                  # an initializer or a default argument
            elif tok == ';' and pending:
                pending = 0                  # forward declaration
            elif tok[0].isalpha() or tok[0] == '_':
                if pending and not parens:
                    idents += 1
            elif tok == '{':
                if pending:
                    stack.append(brace)
                    # Every pending keyword is spent by this brace: a
                    # template-template parameter (`template <class> class
                    # L`) leaves one behind that would otherwise open a
                    # phantom record at the next function body.
                    pending = 0
                brace += 1
            elif tok == '}':
                brace = max(0, brace - 1)
                while stack and brace <= stack[-1]:
                    stack.pop()
    return depths


def main():
    rows = list(csv.DictReader(open(sys.argv[1], newline='')))
    roots = dict(pair.split('=', 1) for pair in sys.argv[2:])

    per_base = defaultdict(Counter)
    # How a field-level annotation is written: on one member, or around a
    # group of them. The kernel writes a group three ways -- an annotated
    # closing brace of a nested anonymous record, an annotated opening one,
    # and __cacheline_group_begin -- and an earlier count saw only the
    # first, which made people look like they mark single boundaries.
    shape = Counter()
    cache = {}
    for r in rows:
        idiom = r['idiom']
        if idiom in ('prose', 'simd_const'):
            continue
        base, rel, line, text = r['codebase'], r['file'], int(r['line']), r['text']
        if idiom == 'cacheline_macro' or text.lstrip().startswith('#'):
            per_base[base]['macro'] += 1
            continue

        root = roots.get(base)
        kind = None
        if root:
            path = str(Path(root) / rel)
            if path not in cache:
                cache[path] = depth_map(path)
            depths = cache[path]
            if depths and 0 < line <= len(depths):
                d = depths[line - 1]        # records enclosing the line's start
                if CLOSES_RECORD.match(text):
                    # The record being closed is itself at depth d-1. If that
                    # is still inside something, this separates fields of it.
                    kind = 'field' if d - 1 >= 1 else 'object'
                else:
                    # A member, or a record opened here: either way it
                    # separates fields exactly when it sits inside a record.
                    kind = 'field' if d >= 1 else 'object'
        if kind is None:
            kind = 'unknown'
        per_base[base][kind] += 1
        if kind == 'field':
            # The kernel also writes `__cacheline_group_begin(x)
            # ____cacheline_aligned;`, which the census files under the
            # aligned idiom; it opens a group all the same.
            if idiom == 'kernel_group' or '__cacheline_group_begin' in text:
                shape['group: begin macro'] += 1
            elif CLOSES_RECORD.match(text):
                shape['group: closing brace'] += 1
            elif OPENS_RECORD.search(text):
                shape['group: opening brace'] += 1
            else:
                shape['one member'] += 1

    order = ['field', 'object', 'macro', 'unknown']
    w = max(len(b) for b in per_base) + 1
    print(f'{"codebase":<{w}}' + ''.join(f'{k:>9}' for k in order) + f'{"total":>8}')
    print('-' * (w + 9 * len(order) + 8))
    tot = Counter()
    for base in sorted(per_base, key=lambda b: -sum(per_base[b].values())):
        c = per_base[base]
        tot.update(c)
        print(f'{base:<{w}}' + ''.join(f'{c[k]:>9}' for k in order) +
              f'{sum(c.values()):>8}')
    print('-' * (w + 9 * len(order) + 8))
    print(f'{"TOTAL":<{w}}' + ''.join(f'{tot[k]:>9}' for k in order) +
          f'{sum(tot.values()):>8}')
    n = tot['field'] + tot['object']
    if n:
        print(f'\nfields separated from fields: {tot["field"]} of {n} '
              f'({100 * tot["field"] / n:.0f}%) -- reordering by owner reaches these')
        print(f'objects separated from objects: {tot["object"]} of {n} '
              f'({100 * tot["object"] / n:.0f}%) -- only the allocation site reaches these')
    f = sum(shape.values())
    if f:
        print(f'\nfield-level annotations by shape, of {f}:')
        for k in sorted(shape):
            print(f'  {k:<22}{shape[k]:>6}  ({100 * shape[k] / f:.0f}%)')


if __name__ == '__main__':
    main()
