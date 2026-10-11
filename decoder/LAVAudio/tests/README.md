# Gain persistence ordering regression

From the repository root:

```
python decoder/LAVAudio/tests/run_gain_persistence_tests.py
```

In a Visual Studio developer shell, use `--compiler cl`. GCC and Clang use
`--compiler g++` (the default) or `--compiler clang++`. Python 3 and C++17 are
required. Build products are placed in a temporary directory and removed.

The runner extracts five **unmodified production method bodies** from
`LAVAudio.cpp`: the gain getter/setter, gain registry loader/saver, and
`SetRuntimeConfig`. The C++ fixture replaces the registry with an in-memory
adapter and the recursive receive critical section with an instrumented
recursive mutex. Its `LoadSettings` adapter omits unrelated decoder settings
but invokes the real gain loader. It does not compile or instantiate the real
DirectShow filter and does not validate Win32 registry APIs or COM behavior.
The native `OpenJocSettingsSmoke` (full and `--output-gain-only`) and packaged
filter tests remain required on Windows.

The first registry write is paused while a second operation attempts the
receive lock. If the second operation acquires it, that operation completes
before the first write resumes; otherwise the first transaction finishes and
releases the lock. This deterministically exercises two setters, enabling
runtime mode, and reloading persisted settings. There are no sleeps or timing
assumptions; five-second watchdogs only detect deadlocks/broken scheduling.
It also checks unchanged/invalid input, flush failure, recursive readback during
flush, runtime-only changes, persisted reload, and registry error propagation.

To reproduce the regression before the fix:

```
git show db288ee0c2e77f1266482c5ff4668378bb4c2cfc:decoder/LAVAudio/LAVAudio.cpp > before.cpp
python decoder/LAVAudio/tests/run_gain_persistence_tests.py --source before.cpp
```

That version fails all three concurrent ordering cases. In particular, two
successful setters leave runtime gain at +12 dB and persisted gain at +6 dB.
The fixed version passes without changing the test or its schedule.

## Locking scope

The gain transaction retains the existing recursive `m_csReceive` through the
registry write. `SetRuntimeConfig` already holds that lock across mode changes,
defaults, and reload. The only other `LoadSettings` caller is construction,
before publication. Generic `SaveSettings` does not write gain. There is no new
lock order, and delivery-related recursive settings readback remains possible.
Registry writes occur only in changed control-setting calls; per-buffer gain
still uses its atomic snapshot. A changed gain save briefly blocks the receive
path, which is the deliberate cost of ordering persistence without introducing
a second lock around potentially reentrant downstream delivery. Gain math,
clipping behavior, and the exact zero-gain bypass are unchanged.

# Output-status fallback regression

From the repository root:

```
python decoder/LAVAudio/tests/run_output_status_fallback_tests.py
```

Python 3 and C++17 are required. `--compiler g++` is the default; Clang uses
`--compiler clang++`, and a Visual Studio developer shell can use `--compiler cl`.
GCC/Clang support `--sanitize` for ASan/UBSan and repeated `--cxxflag=-flag`
options. MSVC uses its own `__popcnt` intrinsic and requires a Visual Studio
developer shell; portable Linux builds do not validate MSVC or native COM
behavior. Generated source and binaries live only in a temporary directory.

The runner extracts the **full, unmodified production bodies** of `QueueOutput`,
`FlushOutput`, `FlushOutputLocked`, `GetOutputDetails`, `Deliver` (both ordinary
and strict JOC branches), `PrepareOpenJocDelivery`, `CompleteOpenJocDelivery`,
`EndOfStream`, and strict media-type creation. It also uses the real strict
media-type builder/validators, delivery transaction, queue transaction,
checked arithmetic, and HRESULT-normalization functions. It prints SHA-256
hashes of every extracted body. GCC/Clang replace only the Microsoft `10000i64`
literal suffix with `10000LL`; a token macro supplies the mocked base class
for `__super` without rewriting the EOS method.

The 46 deterministic cases cover ordinary FP32/accepted-16-bit fallback with a
prior ordinary or JOC type, final flush and EOS, unchanged types, downstream
`S_FALSE`/failure, reconnect/acquisition failure, discard/empty flush, preroll,
5.1-back mask and channel-layout fallbacks, non-native layout masks, optional
getter pointers/bitstream/disconnection, queue append/overflow/failure,
ordinary/strict incompatible queue transitions, and strict JOC
acceptance/rejection/delivery errors/invalid buffers/EOS controls.
A downstream delivery callback reads the queue-published FP32 snapshot while
negotiation has already converted the buffer: status and the volume channel
bound update only after the ordinary downstream `Deliver` returns exactly
`S_OK`. Non-accepted delivery leaves the existing queue-published snapshot.
The legacy ordinary all-candidate-rejection path still attempts delivery with
its last proposed type; that case preserves existing behavior and does not
claim that rejected negotiation succeeded.

Mocked services are portable COM media types/pins/samples, ordinary media-type
construction, allocator/reconnect and acquisition, AV channel-layout routines,
canonical-contract lookup, clock/jitter services, decoder `ProcessBuffer`,
admission refresh, and the base EOS callback. Wave types are structural
stand-ins, not Windows ABI tests. `PerformAVRProcessing` is an explicitly fake,
deterministic byte transform with format/layout metadata changes. The tests
compare copied no-fallback payloads byte-for-byte to their inputs and fallback
payloads byte-for-byte to that transform; `PAYLOAD` lines provide stable lengths
and FNV-1a fingerprints for pre/post comparison. They do **not** validate real
FFmpeg float/integer conversion, mixing, gain, resampling, JOC decoding, or
hardware audio quality. The planar-invalid strict case injects the flag at the
delivery seam, because the real queue method assumes processed/interleaved
input and does not copy that flag.

To reproduce the pre-fix red control on the verified current base:

```
git show 7c1d55aeb3bae52a054ef062a9fabc3b8b41b261:decoder/LAVAudio/LAVAudio.cpp > before.cpp
python decoder/LAVAudio/tests/run_output_status_fallback_tests.py --source before.cpp
```

The same runner can use OpenJOC's pinned `d13b7cac86c5750b5d4181569d98f44ae9a1607c`
`LAVAudio.cpp` via `--source`; `--source-dir` selects the companion strict source
files if needed. Pre-fix runs keep the same byte-payload checks but fail stale
format/mask/channel assertions after successful fallback. Native Windows
DirectShow/filter and renderer checks remain required for runtime confirmation.

# Output-policy persistence ordering regression

```
python decoder/LAVAudio/tests/run_policy_persistence_tests.py
python decoder/LAVAudio/tests/run_policy_persistence_tests.py --cxxflag=-O2
ASAN_OPTIONS=detect_leaks=0 python decoder/LAVAudio/tests/run_policy_persistence_tests.py --sanitize
```

Python 3 and C++17 are required; `--compiler cl` supports a Visual Studio
Developer shell. The portable runner extracts seven unchanged production
methods: output-policy getter, setter, configure, saver and loader, plus
`SetBinauralConfiguration` and `SetRuntimeConfig`. The policy enum/schema are
extracted from the production settings header. SHA-256 hashes of the method
bodies are printed. Build outputs use a temporary directory.

The existing recursive receive lock now covers the entire policy apply/save
transaction, ordering it against other policy changes and runtime-mode
transitions. There is no new lock or lock order. A settings registry write can
briefly block receive, matching the existing gain and property-page settings
transactions. PCM/DSP processing and sample data are unchanged.

A deterministic instrumented recursive mutex pauses the first policy write
while a competing operation attempts the lock. It lets the older write resume
only after detecting that the competitor blocked, or after an unlocked
competitor completed. Five-second watchdogs detect broken schedules; no sleep
or probabilistic race timing determines correctness. Four cases cover two
direct setters, a direct setter versus the property-page API, enabling runtime
mode, and reloading persistent settings. Serial and unchanged policy calls,
runtime-only updates, persistent reload, and getter readback during flush are
also checked. All policy requests are valid 5.1 or 7.1.4 selections.

To verify the old failure without changing the test:

```
git show 010cf396e84d78eb0b8dc3f25c8283259354902f:decoder/LAVAudio/LAVAudio.cpp > before.cpp
python decoder/LAVAudio/tests/run_policy_persistence_tests.py --source before.cpp
```

That source fails all four serialization assertions; the direct and
property-page cases leave live policy 7.1.4 with saved/reloaded policy 5.1.
The runtime-enable case checks exclusion itself because the already-started
registry write also completes after runtime mode is enabled in the old code.

Registry, contract lookup, decoder, flush, and unrelated setting operations are
in-memory adapters. The property-page configuration adapter selects valid
speaker output and invokes the actual policy configuration method; it does not
exercise binaural HRTF loading or SOFA. Runtime `LoadSettings` resets the policy
snapshot and invokes the real policy loader. This is a portable ordering
regression, not native Windows COM, registry, DirectShow, playback, decoder,
ABI, or audio-quality validation. Native Windows build and simultaneous COM
settings calls remain required. Sanitizer runs with leak detection disabled
do not provide leak-detection coverage.
