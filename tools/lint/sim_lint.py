"""Linter of the simulation's sources (mc_sim: code/src/core and code/src/game).

Fails the build when they use what breaks determinism (CLAUDE.md and docs/SIMULATION.md, "Детерминизм") or reach
into the view side. Comments and string and character literals are skipped. Names match on word boundaries, functions
as calls (`pow (` or `std::pow`), so words such as manpower, expected or runtime pass.

    python tools/lint/sim_lint.py code/src
"""

import re
import sys
from pathlib import Path

SIM_DIRS = ('core', 'game')

# The CRT's transcendental functions and fma (with their f/l forms): det_math.h has the simulation's own.
MATH = ('exp', 'exp2', 'expm1', 'log', 'log2', 'log10', 'log1p', 'logb', 'pow', 'cbrt', 'hypot',
        'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh', 'asinh', 'acosh', 'atanh',
        'erf', 'erfc', 'tgamma', 'lgamma', 'fma')
# Randomness, real time, threads, the environment, locale-dependent parsing.
CALLS = ('rand', 'srand', 'rand_s', 'time', 'clock', 'getenv', '_wgetenv', '_dupenv_s', '__rdtsc', '__rdtscp',
         'QueryPerformanceCounter', 'GetTickCount', 'GetTickCount64', 'timeGetTime',
         'atof', 'strtod', 'strtof', 'strtold', 'stod', 'stof', 'stold', 'sscanf')


def function(names):
    """A call of one of `names` (not a member or another namespace's), or std::name used at all."""
    alternatives = '|'.join(re.escape(n) for n in names)
    return re.compile(rf'(?<![\w.>:])(?:std::(?:{alternatives})[fl]?\b|(?:{alternatives})[fl]?\s*\()')


RULES = [
    (re.compile(r'\bunordered_(?:map|set|multimap|multiset)\b'), 'unordered containers (iteration order)'),
    (function(MATH), 'CRT maths or fma (core/det_math.h)'),
    (function(CALLS), 'randomness, real time, the environment or locale-dependent parsing'),
    (re.compile(r'\b(?:random_device|mt19937(?:_64)?|minstd_rand0?|ranlux\w*|knuth_b|default_random_engine)\b'),
     'a standard random engine'),
    (re.compile(r'\b\w+_distribution\b'), 'std::*_distribution'),
    (re.compile(r'\bchrono\b'), 'std::chrono (real time)'),
    (re.compile(r'\bstd::(?:thread|jthread|async)\b'), 'threads outside the deterministic split'),
    (re.compile(r'\b_mm\w*_fn?m(?:add|sub)\w*\b'), 'fused multiply-add intrinsics'),
    (re.compile(r'#\s*include\s*<(?:random|chrono|ctime|time\.h|thread|future|windows\.h|d3d\w*\.h|dxgi\w*\.h|'
                r'imgui\w*\.h)>'), 'a header the simulation must not use'),
]
# Checked on the line as written (code_only blanks the quoted path), when it is an #include.
INCLUDE = re.compile(r'^\s*#\s*include\s*"(?:view|render|app|ui|minecraft|host|editor)/')
INCLUDE_WHAT = 'an include from the view side (the simulation knows no view)'


def code_only(text):
    """`text` with comments and string/character literals blanked out; line breaks are kept."""
    out = []
    i = 0
    n = len(text)

    def blank(chunk):
        return ''.join('\n' if c == '\n' else ' ' for c in chunk)

    while i < n:
        c = text[i]
        if text.startswith('//', i):
            end = text.find('\n', i)
            end = n if end < 0 else end
            out.append(blank(text[i:end]))
            i = end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append(blank(text[i:end]))
            i = end
        elif c == 'R' and text.startswith('"', i + 1) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            open_paren = text.find('(', i + 2)
            delimiter = text[i + 2:open_paren]
            end = text.find(')' + delimiter + '"', open_paren)
            end = n if end < 0 else end + len(delimiter) + 2
            out.append(blank(text[i:end]))
            i = end
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):   # 1'000 is a digit separator
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            end = min(j + 1, n)
            out.append(c + blank(text[i + 1:end - 1]) + c)
            i = end
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def lint(src):
    problems = []
    for folder in SIM_DIRS:
        for path in sorted((src / folder).rglob('*')):
            if path.suffix not in ('.h', '.cpp'):
                continue
            text = path.read_text(encoding='utf-8')
            for number, (raw, line) in enumerate(zip(text.split('\n'), code_only(text).split('\n')), 1):
                for pattern, what in RULES:
                    match = pattern.search(line)
                    if match:
                        problems.append(f'{path}:{number}: {what}: {match.group(0).strip()}')
                if re.match(r'\s*#\s*include', line) and INCLUDE.search(raw):
                    problems.append(f'{path}:{number}: {INCLUDE_WHAT}: {raw.strip()}')
    return problems


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    problems = lint(Path(sys.argv[1]))
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f'sim_lint: {len(problems)} forbidden use(s) in mc_sim (CLAUDE.md, determinism rules)', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
