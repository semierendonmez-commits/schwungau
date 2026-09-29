# Issues found while hosting Schwung modules in a DAW

Hosting modules in a plugin exercises paths a Move rarely does: chain slots are
created and destroyed all the time, several instances load the same module at
once, and state is saved and restored constantly. These are the problems that
surfaced, with enough detail to file upstream. Each was reproduced with
`tests/sw_render` or the loop in "Reproduce" below.

## 1. tablor — use-after-free when an instance is destroyed while its loader runs

**Repo:** athousanddetails/schwung-tablor · **Severity:** crash (SIGSEGV in the
`tablor-wtload` thread) · **On Move:** yes, when tablor is swapped out or its slot
cleared within a second or two of loading it.

`struct tablor_instance` declares `tb::WtLoader loader` *before* `module_dir`,
`wt_path`, the preset tables and everything else that the loader's first posted
job (`tb_create_instance`'s lambda: scan, `load_presets`,
`export_presets_to_schwung`, `tb_publish_selection`, `setTable`) touches.
`tb_destroy_instance` is `delete inst`, and members are destroyed in reverse
order, so those members are gone before `~WtLoader` joins the thread that is
still using them.

Backtrace:

```
Thread "tablor-wtload" SIGSEGV
#0 std::_Sp_counted_base<...>::_M_release_last_use_cold()
#1 tb_create_instance(...)::{lambda()#1}::operator()()
#2 tb::WtLoader::run()
Thread 1
#3 pthread_join
#4 tb::WtLoader::~WtLoader()
#5 tb_destroy_instance(void*)
```

**Fix:** stop and join the loader first — `inst->loader.stop(); delete inst;` —
or declare `loader` as the last member so it is destroyed first.

## 2. scratch — lookup-table race between two instances

**Repo:** mestela/schwung-scratch · **Severity:** abort
(`timecoder.c:240: build_lookup: Assertion ... failed`) · **On Move:** yes, when
two slots hold scratch and load together (e.g. restoring a set at boot).

`timecoder_find_definition("serato_2a")` is called from each instance's loader
thread. `build_lookup` checks `def->lookup` and then fills the process-global
`timecodes[]` LUT, with no lock; two loaders both see `lookup == false` and
the second one trips the "timecode must not wrap" assertion while the first
is still filling the table. `plugin_api_v1.h` calls this out: process-global
initialisation must be thread-safe because the same module can be
constructed on two threads at once.

**Fix:** guard `timecoder_find_definition` / `build_lookup` with a static mutex
(or `pthread_once`).

## 3. Schwung chain host — sub-plugins keep a pointer into a chain instance

**Repo:** charlesvestal/schwung · **Severity:** latent on Move (slots are never
destroyed), use-after-free anywhere a chain instance is destroyed.

`v2_load_synth` / `v2_load_fx` pass `&inst->subplugin_host_api` — storage inside
the chain instance — to each module's `move_plugin_init_v2`, and modules keep
that pointer in a global (`g_host`). Every chain instance loading the same
module re-initialises it, so the global points into whichever instance loaded
it last. When that instance is destroyed, every other instance's copy of the
module holds a dangling `host_api`; the next `host->log()` (moog's destroy log
is the first) is a use-after-free.

The shell works around it by never destroying chain instances (they are
cleared and pooled — Move's lifetime). A fix in the chain host would be to
hand sub-plugins a host_api with static storage duration, shared by all
instances, and keep per-instance context in the callbacks' ctx pointers.

## 4. Patch format depends on key order

Not a bug as such, but worth documenting in `docs/CHAIN.md`: `chain_patch.c`
takes the first `"type"` / `"module"` it meets inside a component's object, so
a writer must emit `type` before `params`. A module whose own state contains a
`"type"` key (the built-in `chord`: `"type":"major"`) otherwise loads a module
called `major`. Shadow UI gets it right because JavaScript keeps insertion
order; any other writer (the desktop shell, scripts, the web manager) has to
know.

## 5. Build scripts: things a non-Move build trips over

Collected from running every module's own script against a desktop toolchain
(see `tools/recipes.json`). None is wrong for Move; each blocks a port.

* 4k-eq, tablor, tape-echo2: `scripts/build.sh` rsyncs to a private VPS over
  ssh; the in-container build is `scripts/docker-build.sh`.
* Scripts that end in a Move-deploy guard (glibc ≤ 2.35, `file` says ARM
  aarch64, no `libmvec`) fail on any other target even when the build worked.
  An env var to skip deploy guards (`SKIP_DEVICE_CHECKS=1`) would help ports.
* branchage, beatbank, groovebank, kit-builder leave `dsp.so` outside
  `dist/<id>/` and never package `module.json` next to it.
* superarp's `build-module.sh` is committed without the exec bit.
* impressive-chords has sources but no build script.

## 6. structor, dissolver — pffft writes past its output buffer (heap corruption, on Move too)

**Repos:** filliformes/structor-move, filliformes/dissolver-move · **Severity:**
silent heap corruption; surfaced as a crash inside `malloc` minutes later ·
**On Move:** yes.

The vendored `pffft.c` in both repos has an operator-precedence change in
`pffft_zreorder` (used by `pffft_transform_ordered`):

```c
reversed_copy(dk, vin + 2, 8, (v4sf *) out + N / 2);   /* vendored  */
reversed_copy(dk, vin + 6, 8, (v4sf *) out + N);
reversed_copy(dk, vin+2, 8, (v4sf*)(out + N/2));       /* upstream  */
reversed_copy(dk, vin+6, 8, (v4sf*)(out + N));
```

`N` counts floats; casting first makes the offset count 4-float vectors, so
the ordered forward transform writes up to 3·N floats beyond `out`. pffft
takes this SIMD path on aarch64 (NEON), x86 (SSE) and Apple silicon alike.
Found with valgrind:

```
Invalid write of size 8
   at reversed_copy.constprop.0 (structor.so)
   by pffft_transform_internal (structor.so)
   by structor_process (structor.so)
 Address ... is 960 bytes inside a block of size 4,096 free'd
```

**Fix:** restore the parentheses. Until then the shell builds both with the
file's own `PFFFT_SIMD_DISABLE` (scalar path, where `v4sf` is `float` and the
expression is correct); valgrind is clean afterwards.

## 7. Minor: uninitialised bytes in saved state (dexed, acid)

A valgrind sweep of every loadable module (each loaded, played, saved and
restored) found no other invalid memory access. Two modules read
uninitialised memory while serialising `state`, so a saved patch can carry
garbage in unused fields; no crash, low priority:

* **dexed** — floats formatted with `printf` in the state writer are read
  before being set (77 reports under `__printf_fp_l_buffer`).
* **acid** (MIDI FX) — `hex_put` encodes bytes of the state struct that were
  never written (padding or unused slots).

Zero-initialising the state structs at creation fixes both.
