# OpenJOC preroll and output format transitions

Negative-time OpenJOC output must consume its timestamp interval without consuming
an output media-type transition. Previously the output pin committed the float
media type before `CompleteOpenJocDelivery` dropped negative preroll. A later
sample could therefore carry float bytes without telling the downstream pin that
its previous integer PCM type had changed.

`DeliverLAVOpenJocStrictMediaType` now calls the required `prepare_delivery`
operation once, before QueryAccept, allocator work, or media-type mutation.
`PrepareOpenJocDelivery` owns the existing resync, fractional duration, jitter,
AutoAVSync and audio-delay calculations. It returns S_FALSE for negative preroll;
the orchestrator reports that consumed/drop outcome as S_OK. The negative-time
check remains after jitter correction and before configured audio delay.
`CompleteOpenJocDelivery` receives already prepared timestamps and cannot discard
preroll or advance the clock a second time.

The sample still carries the exact proposed type before downstream delivery.
The local output-pin type is committed only after downstream returns exactly
S_OK. Failure and S_FALSE preserve the transition for the next consumed buffer.
`ReconnectOutput` only adjusts allocator capacity; it does not commit a media
type. If the local SetMediaType fails after downstream acceptance, the failure is
propagated; a subsequent buffer conservatively attaches the type again.

Preparation now advances the clock even if later negotiation or allocation fails.
This is once per consumed buffer, not an instruction to retry the same buffer:
`FlushOutput` clears its queued buffer on failure as before. No persistent pending
format or timestamp state was introduced, so flush/new-segment/reopen retain the
existing reset behavior. Ordinary PCM delivery, decoding and DSP are unchanged.

## Regression coverage and limits

`OpenJocStrictOutputTests` runs the real production orchestrator with independent
sender and receiver type state: zero/one/multiple negative outputs, first positive
output, acquisition/allocator/delivery failure, S_FALSE delivery, repeated format
switches, same-format seek, no-output contract, and preparation failure. Source
integration assertions ensure the timing/drop operation stays out of final delivery.
These tests must run with assertions enabled; merely building them is not evidence.

A Linux host can exercise this orchestrator with mocked DirectShow types, and can
exercise the extracted production timing method with a mock jitter container.
Such checks do not validate COM, Windows allocators, actual decoder lifecycle,
PotPlayer, physical hardware, or any particular private media file. The Windows
controlled-sink regression is the native boundary check; no physical audio output
is needed or permitted for this test.
