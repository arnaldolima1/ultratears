"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Every patch checks the original instruction word first, so lifting another game version (or a different build)
fails loudly instead of changing the wrong code. Runtime switches live in src/gfx/nvn/hle_nvn.c.
"""


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')


def drs_off(gen, addr):
    """Dynamic resolution off (1.4.3). main+0x584300 is the scene resolution controller: each frame it fits the
    recent frame times and returns {scale (float), state} to the renderer (main+0x581968), which turns the scale into
    the scene size (+0xccc). Its first test is the controller's enable bit (+0x110 bit 0); with the bit clear it
    takes its own disabled path (main+0x584c3c): scale 1.0, history untouched. The patch takes that path unless
    TOTK_DRS=1. Why: the game flips the scene targets between 1600x896 and 1432x808 over the same memory and our
    per-size images then hand stale content to the other size (terrain gone, clouds over the scene);
    image quality is better at a fixed size and upscaling will come from FSR."""
    expect(gen, addr, 0x360042e8, 'drs_off')   # tbz w8, #0, 0x800584c3c
    return ('{ extern int totk_drs_enabled(void); '
            'if (!totk_drs_enabled() || ((c->x[8] >> 0) & 1) == 0) { goto L_800584c3c; } }')


# ---------------------------------------------------------------- 60 FPS (TOTK_FPS60=1)
# Ported from MacNativeGames' games/totk/runtime/game_patches_v143.c (MIT, ly05010419/MacNativeGames @aaf6ba1f49), which
# writes these instructions into the running ARM64 code. Here each site lifts to both versions behind a run-time switch
# (src/host/frame_step.c: totk_fps60()), so the game is unchanged unless the mod is on. The measured frame step
# (totk_step[]: {seconds * 30, seconds, seconds * 60}) replaces the 30 Hz constants the engine steps its time by;
# without it ~60 presents a second would run the game twice as fast.
MAIN = 0x800000000
SET_S = '{{ float f_ = {v}; uint32_t u_; memcpy(&u_, &f_, 4); c->v[{r}] = u_; }}'   # s<r> = v, upper bits cleared
ON = 'totk_fps60()'
DECL = 'extern int totk_fps60(void); extern volatile float totk_step[3]; '


def lifted(gen, addr, word):
    """The instruction `word` as the lifter emits it at addr: C (--simd=c) or an inline-asm block (--simd=asm, ARM64
    hosts: FP/SIMD runs natively on c->v, loaded and stored around the block)."""
    ins = gen.lifter.lift(word, addr)
    if ins.kind == 'asm':
        from lifter import emit_asm_batch
        return emit_asm_batch([ins.asm])
    if ins.kind != 'op':
        raise SystemExit(f'patch at {addr:#x}: {word:#010x} does not lift to plain C ({ins.kind})')
    return ins.code


def fmul_s(d, n, m):
    """fmul s<d>, s<n>, s<m>, lifted like the game's own (either SIMD mode): (gen, addr) -> code."""
    return lambda gen, addr: lifted(gen, addr, 0x1e200800 | m << 16 | n << 5 | d)


def code(*parts):
    """on_code from C strings and (gen, addr) -> code pieces."""
    return lambda gen, addr: ' '.join(p(gen, addr) if callable(p) else p for p in parts)


S32 = '(uint32_t)c->v[{r}]'   # s<r> as its 32 bits


def fps60_swap(new):
    """The frame controller (main+0x2db174) may pick interval 1: seven compares / selects against the maximum
    become compares against 1 (MacNativeGames' fps60 table)."""
    def patch(gen, addr):
        old = gen.word(addr)
        return f'{{ {DECL}if ({ON}) {{ {lifted(gen, addr, new)} }} else {{ {lifted(gen, addr, old)} }} }}'
    return patch


def guarded(word, what, on_code, keep=False):
    """on_code when the mod is on; the original instruction otherwise (keep: after on_code as well)."""
    def patch(gen, addr):
        expect(gen, addr, word, what)
        orig = lifted(gen, addr, word)
        on = on_code(gen, addr) if callable(on_code) else on_code
        if keep:
            return f'{{ {DECL}if ({ON}) {{ {on} }} {orig} }}'
        return f'{{ {DECL}if ({ON}) {{ {on} }} else {{ {orig} }} }}'
    return patch


FPS60_CONTROLLER = {   # offset: (original, replacement)
    0x2db204: (0x6b08015f, 0x7100055f), 0x2db208: (0x1a89b10a, 0x1a9fa52a), 0x2db22c: (0xb940468a, 0xd503201f),
    0x2db340: (0xb9400288, 0x52800028), 0x2db34c: (0x2a0a03f5, 0x1a9fa555), 0x2db360: (0x6b0801bf, 0x710005bf),
    0x2db36c: (0x1a88c1ac, 0x1a9fc5ac),
}


def controller(off):
    old, new = FPS60_CONTROLLER[off]
    def patch(gen, addr):
        expect(gen, addr, old, 'fps60 controller')
        return fps60_swap(new)(gen, addr)
    return patch


ratio, seconds, interval = 'totk_step[0]', 'totk_step[1]', 'totk_step[2]'
FPS60 = {
    # The 30 Hz ratio the engine derives from its FPS setting (60 / fps * 0.5 at main+0x2daa54..78) is the measured
    # one; s1 keeps the original's final 0.5.
    0x2daa54: guarded(0xbd425260, 'realtime ratio', SET_S.format(r=0, v=ratio)),
    0x2daa6c: guarded(0x5e21d800, 'realtime scvtf', ''),
    0x2daa70: guarded(0x1e201820, 'realtime fdiv', ''),
    0x2daa78: guarded(0x1e210800, 'realtime fmul', ''),
    # The nominal 60 / FPS interval before FrameRateMgr's 0.5 / 1.0 selector (fmul s8, s0, s1) is the measured one.
    0x3e5658: guarded(0x1e210808, 'realtime interval', SET_S.format(r=0, v=interval), keep=True),
    # Cloth / hair: the actor animation's second clock built {factor / 30, factor} per rendered frame.
    0x246eac: guarded(0x1e200940, 'cloth clock 1',
                      code(SET_S.format(r=0, v=ratio), fmul_s(0, 10, 0), f'ST32(c->sp + 0x364ull, {S32.format(r=0)});',
                           SET_S.format(r=0, v=seconds), fmul_s(0, 10, 0))),
    0x345904: guarded(0x1e210801, 'cloth clock 2',
                      code(SET_S.format(r=1, v=ratio), fmul_s(1, 0, 1), f'ST32(c->sp + 0x114ull, {S32.format(r=1)});',
                           SET_S.format(r=1, v=seconds), fmul_s(1, 0, 1))),
    # ... and the two callers of the out-of-line update at main+0x2a0e40 (s2 / s3 scaled before the bl that follows;
    # its third caller already passes measured time)
    0x249448: guarded(0x2a1403e6, 'cloth call 1',
                      code(SET_S.format(r=4, v=ratio), fmul_s(2, 2, 4), fmul_s(3, 3, 4)), keep=True),
    0x28bc94: guarded(0x2a1a03e6, 'cloth call 2',
                      code(SET_S.format(r=4, v=ratio), fmul_s(2, 2, 4), fmul_s(3, 3, 4)), keep=True),
    # Trampolines: the bounce is cut to 0.1 when the time object's FPS (X+0x28) > 100, meant as "time slowed below
    # 0.3"; compare FPS * ratio (= 30 / time scale) instead, independent of the frame rate.
    0x562a64: guarded(0xbd402920, 'bounce check',
                      code('{ uint64_t x_ = c->x[9]; c->v[0] = LD32(x_ + 0x28ull); c->v[1] = LD32(x_ + 0x38ull); }',
                           fmul_s(0, 0, 1))),
}


# ---------------------------------------------------------------- scene render resolution (TOTK_RENDER_RES)
# MacNativeGames' patch_render_res (game_patches_v143.c, MIT): every path that picks the scene size converges at
# main+0x585e48, which converts the mode entry's width and height (w11 / w10) and stores them to the renderer's
# +0xcc4 / +0xcc8. With the mod on the stores take src/host/render_res.c's size; s0 is set again at main+0x585e64.
def render_store(word, what, index, offset):
    def patch(gen, addr):
        expect(gen, addr, word, what)
        orig = lifted(gen, addr, word)
        return ('{ extern int totk_render_res(void); extern volatile float totk_render_size[2]; '
                f'if (totk_render_res()) {{ float f_ = totk_render_size[{index}]; uint32_t u_; memcpy(&u_, &f_, 4); '
                f'ST32(c->x[19] + {offset:#x}ull, u_); }} else {{ {orig} }} }}')
    return patch


PATCHES = {
    0x8005843e0: drs_off,
    MAIN + 0x585e50: render_store(0xbd0cc660, 'render width', 0, 0xcc4),
    MAIN + 0x585e60: render_store(0xbd0cca60, 'render height', 1, 0xcc8),
    **{MAIN + off: controller(off) for off in FPS60_CONTROLLER},
    **{MAIN + off: p for off, p in FPS60.items()},
}
