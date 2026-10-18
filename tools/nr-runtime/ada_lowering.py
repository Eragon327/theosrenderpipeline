"""NVIDIA NR 310.9.16 PTX 9.4 (sm_120) -> Ada (sm_89) lowering for modules that use
sm_90+ features. Used by build_rtx40.py; run with python -I.

No CUDA driver/runtime APIs are used. This is a deliberately narrow, fail-closed
source transformation, not a general PTX translator. Review new source patterns
before extending it. Full warps and warp-uniform bulk bases/predicates are launch
contracts inherited from the original kernels.
"""
import collections
import re


def require(condition, message):
    # Unlike assert, never removed by python -O.
    if not condition:
        raise RuntimeError(message)

EXPECTED = {
    4: dict(bulk=104, elect=104, init=59, expect=104, arrive=46, wait=46, red=0, relu=0, fence=0),
    5: dict(bulk=208, elect=208, init=61, expect=208, arrive=58, wait=58, red=176, relu=0, fence=22),
    6: dict(bulk=4, elect=4, init=4, expect=4, arrive=4, wait=4, red=16, relu=0, fence=0),
    13: dict(bulk=0, elect=0, init=0, expect=0, arrive=0, wait=0, red=0, relu=4, fence=0),
}


def math_stream(s):
    # These exact ordered instruction lists (including operands/rounding) must
    # survive. Integer address/loop arithmetic is also left untouched by lowering.
    pattern = (r'(?<![\w.])(?:mma\.[^;]+|cvt\.[^;]+|'
               r'(?:add|sub|mul|mad|fma|div|sqrt|rsqrt|rcp|ex2|lg2|sin|cos|min|max|abs|neg|setp)'
               r'[.\w]*\.f(?:16|32|64)[^;]*);')
    return [' '.join(x.split()) for x in re.findall(pattern, s)]


def lower(s, module):
    counts = collections.Counter()
    sizes = collections.Counter()
    require(s.count('.target sm_120') == 1, f'module {module}: expected one sm_120 target')
    s = s.replace('.target sm_120', '.target sm_89')
    original_math = math_stream(s)

    # Election guards ONLY bulk-copy paths in this version. All 32 lanes now
    # execute the path to copy disjoint pieces. A lane-0 replacement would lose
    # bandwidth or require a serial 32-iteration loop; it is not appropriate here.
    def elect(m):
        counts['elect'] += 1
        prefix=s[max(0,m.start()-200):m.start()]
        require(re.search(r'mov\.b32 '+re.escape(m[1])+r', -1;',prefix), 'Non-full-warp election')
        return '// PORT: cooperative warp copy; all lanes participate.\nmov.pred P_OUT, 1;'
    s = re.sub(r'elect\.sync _\|P_OUT, (%r\d+);', elect, s)

    bulk_pattern = re.compile(
        r'cp\.async\.bulk\.shared::cta\.global\.mbarrier::complete_tx::bytes '
        r'\[(%r\d+)\], \[(%rd\d+)\], (%r\d+), \[(%r\d+)\];')
    def bulk(m):
        counts['bulk'] += 1
        dst, src, size_reg, barrier = m.groups()
        # Size is an immediate mov immediately before the inline asm; reject
        # unexpected runtime-sized or different transfers rather than guessing.
        prefix = s[max(0, m.start()-300):m.start()]
        size_match = re.search(r'mov\.b32 '+re.escape(size_reg)+r', (512|1024);\s*$', prefix)
        require(size_match, f'module {module}: unknown bulk size in {m.group()}')
        size = int(size_match[1]); sizes[size] += 1
        covered = [lane*16+512*part+byte for part in range(size//512)
                   for lane in range(32) for byte in range(16)]
        require(sorted(covered) == list(range(size)), 'lane slices do not cover the bulk copy')
        instructions = [
            '// PORT: original %d-byte bulk copy; 16 bytes per lane per slice.' % size,
            '{', '.reg .u32 port_lane, port_off, port_dst;',
            '.reg .u64 port_off64, port_src;',
            'mov.u32 port_lane, %laneid;',
            'shl.b32 port_off, port_lane, 4;',
            'mul.wide.u32 port_off64, port_lane, 16;',
            f'add.u32 port_dst, {dst}, port_off;',
            f'add.u64 port_src, {src}, port_off64;',
            'cp.async.ca.shared.global [port_dst], [port_src], 16;',
        ]
        if size == 1024:
            instructions += [
                'add.u32 port_dst, port_dst, 512;',
                'add.u64 port_src, port_src, 512;',
                'cp.async.ca.shared.global [port_dst], [port_src], 16;',
            ]
        return '\n'.join(instructions + ['}'])
    s = bulk_pattern.sub(bulk, s)

    def remove(kind):
        def f(m):
            counts[kind] += 1
            return '// PORT: removed transaction barrier ' + kind + '.'
        return f
    s = re.sub(r'mbarrier\.init\.shared\.b64 \[%r\d+\], %r\d+;', remove('init'), s)
    s = re.sub(r'mbarrier\.expect_tx\.relaxed\.cta\.shared::cta\.b64 \[%r\d+\], %r\d+;', remove('expect'), s)

    # Arrives are at the original CTA-wide stage-completion points, after the
    # compute that overlaps the next-stage copy. Each lane completes its OWN
    # cp.async operations; bar.sync publishes all warp copies and OOB zero stores
    # to every CTA consumer. Tokens and wait loops become trivially satisfied.
    def arrive(m):
        counts['arrive'] += 1
        prefix=s[max(0,m.start()-200):m.start()]
        require(re.search(r'mov\.b32 '+re.escape(m[2])+r', 1;\s*$',prefix), 'Non-unit arrival')
        lines = ['// PORT: original stage boundary; preserve prior copy/compute overlap.',
                 'cp.async.commit_group;']
        lines += ['cp.async.wait_group 0;', 'bar.sync 0;']
        lines += [f'mov.b64 {m[1]}, 0;']
        return '\n'.join(lines)
    s = re.sub(r'mbarrier\.arrive\.shared::cta\.b64 (%rd\d+), \[%r\d+\], (%r\d+);', arrive, s)
    def wait(m):
        counts['wait'] += 1
        return 'mov.pred P_OUT, 1; // PORT: stage completed at arrive.'
    s = re.sub(r'mbarrier\.try_wait\.shared::cta\.b64 P_OUT, \[%r\d+\], %rd\d+;', wait, s)

    def red(m):
        counts['red'] += 1
        pred, addr, values = m.groups()
        pred = pred or ''
        values = [x.strip() for x in values.split(',')]
        require(len(values) == 4, 'expected a four-element vector reduction')
        return '\n'.join(
            f'{pred}red.global.add.noftz.f16x2 [{addr}{"+"+str(4*i) if i else ""}], {v};'
            for i,v in enumerate(values))
    s = re.sub(r'(?m)^(@\S+\s+)?red\.global\.v4\.f16x2\.add\.noftz \[(%rd\d+)\], \{([^}]+)\};',
               red, s)

    def relu(m):
        counts['relu'] += 1
        dst, a, b = m.groups()
        return f'min.s32 {dst}, {a}, {b};\nmax.s32 {dst}, {dst}, 0;'
    s = re.sub(r'min\.relu\.s32 (%r\d+), (%r\d+), (%r\d+);', relu, s)
    def fence(m):
        counts['fence'] += 1
        return 'fence.acq_rel.gpu; // PORT: strengthen original '+m[1]+' fence.'
    s = re.sub(r'fence\.(acquire|release)\.gpu;', fence, s)
    for k, n in EXPECTED[module].items():
        require(counts[k] == n, f'module {module}: {counts[k]} {k} rewrites, expected {n}')
    require(not re.search(r'^(?!//).*(?:cp\.async\.bulk|mbarrier\.|elect\.sync|min\.relu|red\.global\.v[24])',s,re.M),
            f'module {module}: unsupported instructions remain')
    require(math_stream(s) == original_math, f'module {module}: numerical instruction stream changed')
    return s, dict(counts), dict(sizes), original_math
