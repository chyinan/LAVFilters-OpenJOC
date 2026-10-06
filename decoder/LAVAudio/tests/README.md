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
