#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
"""Add farland's compositor patches to a distribution's PKGBUILD.

    python3 packaging/patch-pkgbuild.py PKGBUILD PKGREL_SUFFIX PATCH...

The patch files sit beside the PKGBUILD. They join source=(), their
checksums are skipped because they are ours rather than downloaded, pkgrel
gains the suffix so the package outranks the distribution's own build of the
same version, and prepare() applies them.
"""

import re
import sys


def add_to_array(text, name, values):
    """Append values inside an existing `name=(...)` array."""
    opening = re.search(rf'^{name}=\(', text, re.M)
    if not opening:
        raise SystemExit(f'the PKGBUILD has no {name}=()')
    depth, i = 0, opening.end() - 1
    while i < len(text):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    else:
        raise SystemExit(f'the PKGBUILD has an unclosed {name}=()')
    added = ''.join(f'\n        {value}' for value in values)
    return text[:i] + added + '\n' + text[i:]


def closing_brace(text, opening):
    """The index of the `}` that closes the `{` this match ends with."""
    depth, i = 0, opening.end() - 1
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise SystemExit('the PKGBUILD has an unclosed function')


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    path, suffix, patches = argv[1], argv[2], argv[3:]

    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()

    text = add_to_array(text, 'source', [f"'{p}'" for p in patches])
    for sums in ('b2sums', 'sha256sums', 'sha512sums', 'md5sums'):
        if re.search(rf'^{sums}=\(', text, re.M):
            text = add_to_array(text, sums, ["'SKIP'"] * len(patches))

    text, count = re.subn(r'^pkgrel=(.*)$', lambda m: f'pkgrel={m.group(1)}{suffix}',
                          text, count=1, flags=re.M)
    if not count:
        raise SystemExit('the PKGBUILD has no pkgrel')

    applies = ''.join(f'\n    patch -Np1 -i "$srcdir/{p}"' for p in patches)
    prepare = re.search(r'^prepare\(\)\s*\{', text, re.M)
    if prepare:
        # At the *end* of prepare(): the distribution's own begins by changing
        # into the source tree, and a patch applied before that runs in
        # $srcdir, where there is nothing to patch.
        end = closing_brace(text, prepare)
        text = text[:end] + applies.lstrip('\n') + '\n' + text[end:]
    else:
        build = re.search(r'^build\(\)\s*\{', text, re.M)
        if not build:
            raise SystemExit('the PKGBUILD has neither prepare() nor build()')
        # No prepare() to borrow a directory from, so this one changes into
        # the source tree itself.
        block = ('prepare() {\n    cd "$srcdir"/*/' + applies + '\n}\n\n')
        text = text[:build.start()] + block + text[build.start():]

    with open(path, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(text)
    print('added:', ' '.join(patches))


if __name__ == '__main__':
    main(sys.argv)
