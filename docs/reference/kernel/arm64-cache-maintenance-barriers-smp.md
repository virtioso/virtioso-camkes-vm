# ARM64 cache maintenance vs barriers in SMP (seL4-flavored mental model)

You’ve discovered the core truth the hard way: **cache maintenance instructions move data**, but **barriers make those moves *count*** (ordered, completed, and visible to the observers you care about).

This note focuses on AArch64 (ARMv8+), SMP, and the VA-based ops you mentioned (`dc cvac`, `dc civac`, `dc cvau`), assuming you’re intentionally avoiding set/way (`dc cisw`, etc.) sequences.

---

## 1) The three “locations” you keep juggling: PoU, PoC, and “somebody else’s cache”

### Point of Unification (PoU)
A *unification* point is where the **instruction side** and **data side** agree.  
`dc cvau` (“clean by VA to PoU”) exists mainly to support **I-cache coherence on the *local* core** after writing code/data that will be executed as instructions.

### Point of Coherency (PoC)
A *coherency* point is where **all observers that participate in coherency** can agree on a single value.

- `dc cvac` cleans a line **to PoC** (write back, keep the line valid).
- `dc civac` cleans **and invalidates** the line, also **to PoC** (write back + discard).

**Key SMP consequence:** Cleaning to PoC ensures the *memory system* has the updated bytes, but it does **not** magically reach into other cores and delete/refresh their private cachelines unless the system is actually coherent (and even then, what happens depends on coherence protocol and the line’s state).

So:
- If your system is truly cache-coherent for that memory (typical for “Normal, Inner Shareable” RAM), you *usually* don’t need explicit cache maintenance for CPU↔CPU sharing—**barriers + atomics** are the tool.
- If you *do* need explicit maintenance (boot stages, weird memory types, non-coherent fabrics, or kernel design choices), then **PoC is the safe target** for cross-core visibility.

---

## 2) Barriers: what they do (and what they don’t)

### DMB — Data Memory Barrier (ordering only)
`dmb <domain>` orders **memory accesses** before/after it.  
It does **not** guarantee completion of cache maintenance effects.

Use `dmb` for “this store must be observed before that store/load” logic (locks, release/acquire patterns), not to “finish” cache ops.

### DSB — Data Synchronization Barrier (ordering + completion)
`dsb <domain>` orders and **waits until completion** of:
- prior memory accesses (loads/stores), and
- **maintenance operations** (cache / TLB / etc.), *as visible in the specified domain*.

For cache maintenance, **DSB is the “make it real” instruction**.

### ISB — Instruction Synchronization Barrier (pipeline/stream restart)
`isb` flushes the pipeline so subsequent instructions are fetched/decoded using the **new architectural state**.

Most commonly paired with:
- instruction cache invalidation,
- changes to translation/permissions, or
- writes to certain system registers.

---

## 3) Barrier “scope” suffixes: `ish`, `osh`, `sy`, … (this matters a lot)

Barriers come with a *domain* operand describing **which observers must be synchronized**:

- `ish` — *Inner Shareable*: typically “all cores in the coherency domain for Normal Inner-Shareable memory”.
- `osh` — *Outer Shareable*: extends further out (often relevant for I/O / DMA observers).
- `nsh` — *Non-shareable*: essentially local-ish, rarely correct for SMP shared RAM.
- `sy` — “Full system” (strongest; usually slowest).

And access-type refinements:
- `ishst` — order/wait for **stores** only (cheaper than full).
- `ishld` — order/wait for **loads** only.

**Rule of thumb (practical kernel work):**
- **CPU↔CPU in shared RAM:** `dsb ish` / `dmb ish` are the usual tools.
- **CPU↔device (non-coherent DMA):** you often need `dsb osh` (or `dsb sy`) around the handoff points.

If your page tables mark shared RAM as **Inner Shareable** (common), then `ish` is the right default. If the memory is mapped *Non-shareable*, even “perfect” barriers won’t produce cross-core ordering guarantees.

---

## 4) Cache maintenance is weakly ordered — you must “fence” it

Architecturally, cache maintenance ops are **not strongly ordered** with regular memory accesses. That’s why the standard safe patterns look like:

- **Before** maintenance: ensure prior stores/loads are where you think they are (often a DSB variant).
- **After** maintenance: ensure the maintenance is complete before you proceed (DSB).

In other words: *cache ops without DSB are like telling a raccoon to “eventually” clean your kitchen.*

---

## 5) Canonical “recipes” you can paste into your brain

### 5.1 “Flush this region so another observer can safely read it” (write-back + optional drop)
This is the generic “producer makes data visible” operation.

**If you want *maximum safety* (and can afford it):** use `dc civac` (clean+invalidate).  
**If you want to keep your cache warm:** use `dc cvac` (clean only).

**Pattern (CPU↔CPU, Inner Shareable):**
```asm
dsb   ishst            // push prior stores out of the store buffer
// loop over cache lines:
dc    cvac,  Xt        // or dc civac, Xt
dsb   ish              // completion of the clean/flush in inner shareable domain
```

**Pattern (CPU↔Device, conservative):**
```asm
dsb   oshst            // ensure prior stores are observable for outer-shareable observers
// loop over lines:
dc    cvac, Xt         // or civac if you want to drop dirty lines too
dsb   osh              // ensure completion before ringing a device doorbell
```

Notes:
- Use `osh*` if the “other observer” is outside the CPU inner-shareable domain (common for DMA).
- Use `sy` only if you have reason to believe `osh` isn’t sufficient on your platform.

---

### 5.2 “I need to read data that some other agent wrote into RAM” (invalidate)
This is the “consumer” direction: **discard stale cachelines** so you fetch fresh data.

**Important safety rule:** never do a pure invalidate on lines that might be dirty (you could lose data).  
If you’re not *certain* the lines are clean, `dc civac` is the “safe hammer”.

**Pattern (after DMA writes, CPU consumes):**
```asm
dsb   osh              // make sure the device’s writes are globally visible
// loop:
dc    ivac, Xt         // discard stale lines (only if you're sure no dirty lines exist)
dsb   ish              // ensure invalidation completes before the CPU loads
```

**If you cannot prove cleanliness:**
```asm
dsb   osh
// loop:
dc    civac, Xt        // clean+invalidate is safe even if lines are dirty
dsb   ish
```

---

### 5.3 Self-modifying code / “write code then execute it”
This is where PoU historically shows up.

**Same core executes what it just wrote:**
```asm
// write new instructions into memory...
dsb   ishst
// loop:
dc    cvau, Xt         // clean to PoU (for I-side visibility)
dsb   ish
// loop:
ic    ivau, Xt         // invalidate I-cache by VA to PoU
dsb   ish
isb                   // restart fetch/decode so new instructions are used
```

**Different core will execute the code you generated:**
- You must make the bytes visible at **PoC** (not merely PoU),
- then ensure the target core(s) invalidate their I-cache for that region (often via IPI).

A typical cross-core sequence is:

**Producer core:**
```asm
// write code...
dsb   ishst
// loop:
dc    cvac, Xt         // clean to PoC (not cvau)
dsb   ish              // ensure completion before signaling other cores
```

**Then signal target core(s)** (IPI or some seL4 mechanism).

**Target core(s):**
```asm
// loop:
ic    ivau, Xt
dsb   ish
isb
```

Why? Because PoU is not a “global meeting point” across cores. PoC is the correct rendezvous for shared memory contents.

---

## 6) Why you saw “PoU → PoC” reduce SMP crashes

Common failure modes when PoU is used where PoC is required:

- **Another core (or a DMA-capable unit) reads old bytes** because the modified data never reached the coherency point visible to that observer.
- **Page-table walkers / hardware engines** observe stale descriptors because the updated cachelines weren’t pushed far enough.
- **Instruction fetch on a *different* core** sees old code because the producer only cleaned to PoU and didn’t coordinate I-cache invalidation remotely.

Switching to PoC tends to “paper over” these because it forces data further out into the shared memory system.

It’s not a free lunch: you might now be doing *more* work than strictly necessary on a fully coherent system. But for correctness while you’re still mapping the platform’s true coherency story, PoC is a good default.

---

## 7) “Set/way cache ops don’t work for us” — what to do instead (and why it’s plausible)

Even though set/way operations are architected, in real systems they are fragile because they depend on:
- correct cache geometry enumeration (CLIDR/CCSIDR/CSSELR sequencing),
- correct cache level iteration (LoC/LoUIS/LoUU distinctions),
- required barriers between CSSELR selection and maintenance,
- interactions with virtualization/security modes, and
- SoC-specific errata.

VA-based ops (`dc cvac`, `dc civac`, `dc ivac`, …) are **less error-prone** because they:
- don’t require you to understand the cache topology,
- target exactly the lines you care about,
- naturally compose with “flush this region” style APIs.

For seL4 specifically, VA-based maintenance is usually the better long-term engineering choice.

---

## 8) Practical guidance for seL4 SMP: how to choose barriers

### A) Default barrier pairing for VA-based `dc` loops
If you don’t want to be clever yet:

- **Before** a flush that depends on earlier stores: `dsb ishst`
- **After** the loop: `dsb ish`

This is “correct and boring”, which is a compliment in kernels.

### B) Use `osh` when synchronizing with DMA / external agents
If the maintenance is specifically to make buffers visible to a device (or to see what a device wrote), upgrade domain to `osh` (or `sy` if you must):

- `dsb oshst` before handing data *to* the device
- `dsb osh` after flushing, before ringing doorbell
- `dsb osh` before invalidating for data *from* the device

### C) Use `isb` only when the instruction stream must be reloaded
If you changed something that affects instruction fetch/execute, e.g.:
- new code,
- permissions / translation,
- I-cache invalidation,

…then `isb` is non-negotiable.

---

## 9) A small “cheat sheet” (common sequences)

### Flush D-cache lines to PoC (keep valid)
```asm
dsb ishst
dc  cvac, Xt   // per line
dsb ish
```

### Flush D-cache lines to PoC (clean + invalidate)
```asm
dsb ishst
dc  civac, Xt  // per line
dsb ish
```

### Invalidate D-cache lines (only if you know lines are not dirty)
```asm
dsb ish
dc  ivac, Xt   // per line
dsb ish
```

### D-cache → I-cache coherency on same core
```asm
dsb ishst
dc  cvau, Xt   // per line
dsb ish
ic  ivau, Xt   // per line
dsb ish
isb
```

### Cross-core: generated code executed elsewhere
Producer:
```asm
dsb ishst
dc  cvac, Xt   // per line
dsb ish
// signal other core(s)
```
Receiver:
```asm
ic  ivau, Xt   // per line
dsb ish
isb
```

---

## 10) Pitfalls that look like “random SMP crashes”

1. **Wrong shareability in page tables**
   - If kernel shared structures are mapped `Non-shareable`, no amount of `ish` barriers will give you SMP semantics you expect.

2. **Invalidate when dirty**
   - `dc ivac` can silently drop dirty data. Use `civac` unless you can prove safety.

3. **Missing the “after” DSB**
   - Cache ops can be pending; without `dsb`, your code may race its own maintenance.

4. **PoU used for cross-core visibility**
   - Works “sometimes”, fails “mysteriously”. SMP loves these bugs.

5. **Not aligning/stepping by the real cache line size**
   - Use `CTR_EL0.DminLine` / `CTR_EL0.IminLine` to compute line size and iterate correctly.

---

## 11) A pragmatic seL4 stance while you stabilize SMP

If your goal right now is “stop crashing, then optimize later”, a sane conservative policy is:

- Prefer **PoC** (`cvac` / `civac`) for any maintenance that affects shared kernel objects, page tables, IPC buffers, or anything another core might touch.
- Use `dsb ishst` before + `dsb ish` after VA-based maintenance loops.
- Use `osh` domains when the other observer is a device.
- Reserve `cvau` for the specific “D→I coherency on *this* core” use case; for cross-core code execution, push to PoC and shoot down I-cache on the target core(s).

This is not the fastest approach. It *is* the one that tends to produce a working kernel on complicated SoCs.

---

### Closing nerdy note
Barriers are not “extra seasoning” you sprinkle after cache ops. They’re the **contract language** that turns “I asked the hardware nicely” into “the hardware is obligated to have done it now, for the observers I care about.”

