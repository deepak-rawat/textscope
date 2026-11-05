# TextScope feature roadmap

Potential improvements, not delivery promises. Current behavior is documented
in the [user guide](USER_GUIDE.md).

## Accessibility and interaction

- [ ] Virtual text/row accessibility for screen-reader navigation.
- [ ] Improved focus, high-DPI and keyboard-layout support.
- [ ] Better cross-application filter/text/HTML clipboard interchange.

## Large-file performance and resilience

- [ ] Compact line indexes with 64-bit offsets, random access and disk spill.
- [ ] Faster fragmented-set composition and lower predicate-cache storage costs.
- [ ] Multi-core matching and optional SIMD/multi-pattern acceleration.
- [ ] Lower transient memory use for very long lines.
- [ ] Clearer low-disk/I/O errors and recovery of crash-orphan temporary files.

## Optional extensions

Require explicit scope and demonstrated need:

- Live tail/follow and incremental source updates.
- Tabs, saved sessions and persistent index caches.
- Charts, boolean filter expressions and JSON formatting.
- Built-in compressed-file/archive parsing.
- Process isolation for untrusted or unstable native parsers.
- Original .NET preferences import or legacy parser adapters.
- Optional .NET regex compatibility; PCRE2 remains the current engine.
