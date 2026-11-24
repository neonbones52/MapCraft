"""Linter of the layer graph of code/src (CLAUDE.md, "Архитектура").

Fails the build when a layer includes a layer it must not know, or a library header that belongs to other layers
(D3D, ImGui, Windows, the asset libraries). The table below is the graph: a new folder in code/src, or a new edge,
is a decision made here and in CLAUDE.md together. The simulation's own rules (determinism) are tools/lint/sim_lint.py.

    python tools/lint/layers.py code/src
"""

import re
import sys
from pathlib import Path

# Layer -> the layers it may include (besides itself).
LAYERS = {
    'core': set(),
    'game': {'core'},
    'view': {'core', 'game'},
    'minecraft': set(),
    'render': {'core', 'view'},
    'ui': {'core', 'game', 'view', 'minecraft'},
    'app': {'core', 'game', 'view', 'minecraft', 'render', 'ui'},
    'host': {'core', 'view', 'minecraft', 'app'},
    'editor': {'core', 'game', 'view', 'minecraft', 'render', 'ui', 'app'},
}

# Library headers -> the layers that may include them.
GRAPHICS = {'render', 'ui', 'app', 'host', 'editor'}
LIBRARIES = [
    (re.compile(r'^(?:d3d\w*|dxgi\w*)\.h$|^wrl/'), GRAPHICS, 'D3D'),
    (re.compile(r'^imgui(?:_internal)?\.h$|^imgui_impl_dx11\.h$'), {'ui', 'editor'}, 'ImGui'),
    (re.compile(r'^imgui_impl_win32\.h$'), {'host', 'editor'}, 'the ImGui Win32 backend (platform)'),
    (re.compile(r'^(?:windows|shellapi|wincodec|objbase|combaseapi|shlobj\w*|winuser|commdlg)\.h$', re.I),
     {'host', 'editor'}, 'Windows (platform)'),
    (re.compile(r'^(?:miniz\.h|nlohmann/|stb_\w+\.h$)'), {'minecraft'}, 'an asset library (zip, JSON, PNG)'),
]

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def lint(src):
    problems = []
    for path in sorted(src.rglob('*')):
        if path.suffix not in ('.h', '.cpp'):
            continue
        relative = path.relative_to(src)
        layer = relative.parts[0] if len(relative.parts) > 1 else ''
        if layer not in LAYERS:
            problems.append(f'{path}: not in a known layer (add it to tools/lint/layers.py and CLAUDE.md)')
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8').split('\n'), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            quote, name = match.groups()
            if quote == '"':
                target = name.split('/')[0]
                if target != layer and target in LAYERS and target not in LAYERS[layer]:
                    problems.append(f'{path}:{number}: {layer}/ must not include {target}/: {name}')
                continue
            for pattern, allowed, what in LIBRARIES:
                if pattern.search(name) and layer not in allowed:
                    problems.append(f'{path}:{number}: {layer}/ must not use {what}: <{name}>')
    return problems


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    problems = lint(Path(sys.argv[1]))
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f'layers: {len(problems)} include(s) across the layer graph (CLAUDE.md, "Архитектура")', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
