# 311 — the MSAA colour-surface reproducers

Why browser pages are white: `getContext('webgl')` defaults to
`antialias: true`, which needs a multisampled **colour** render target, and
`heapTextureSizeAndAlignWithDescriptor:` returns **size 0** for one on this
chip. Multisampled **depth** sizes and allocates fine. Full record: `311` in
`docs/JOURNAL.md`.

## The Metal side — no browser, no boot, no sudo

    clang -fobjc-arc -framework Metal -framework Foundation -o /tmp/sz sz.m && /tmp/sz

`sz.m` is the decisive one: sizing versus allocation, with a single-sample
colour control and an MSAA depth control in the same run.
`mtx.m` is the descriptor matrix (formats, sample counts, sizes, array).
`msaa.m` renders into an MSAA target and resolves it.

**Both `MTLStorageModeMemoryless` and `Shared`/`Managed` raise an assertion
for multisample textures on this device and abort the process**, so a row
using them truncates every row after it — which reads as a short table, not
as an error. Keep them out.

## The WebGL side

    python3 -I serve.py          # 127.0.0.1:8731, collects POSTed results
    open -a Safari http://127.0.0.1:8731/probe3.html
    cat result.txt

`probe3.html` is the one that matters: it varies `antialias` alone,
interleaved, repeated, at two canvas sizes. `probe.html` and `probe2.html`
are kept because they are the trap — they differ in several attributes at
once and together they look like "WebGL is completely broken" and "WebGL is
completely fine" (rule 17). `probe4.html` tests our own multisample
renderbuffers.

A page cannot write a file, which is why the result comes back over POST.
