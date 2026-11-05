# Core benchmark

Use the C++20 Release build described in the
[developer guide](../docs/DEVELOPER_GUIDE.md#performance-measurements).
The `textscope_benchmark` target links the production core engine. Run:

```
textscope_benchmark --generate-mib 64 --budget-mib 1
textscope_benchmark --generate-mib 10240
textscope_benchmark --file /path/to/existing.log
```

The generated input is privately owned and removed afterward. Existing inputs
are read only. `--file` and `--generate-mib` are mutually exclusive. Temporary
storage uses the writable current directory. The default generated size is
64 MiB; the default snapshot RAM budget is 32 MiB. Zero budget forces spill.

A single JSON object reports full immutable capture plus indexing time, an isolated
single ASCII-literal scan (`singleLiteralMs`, with explicit case-sensitivity,
accepted count, and scanned bytes), one bulk literal/regex scan, a cache-only
color/enabled-state recomposition, source
bytes and line counts, accepted counts, logical scanned bytes, compact index size,
estimated snapshot/set resident storage, and process peak RSS. Timings are actual
QElapsedTimer measurements in milliseconds; input generation is excluded.
Resident estimates exclude transient decode and PCRE2 buffers; process peak RSS
includes them and library overhead. Full cache hits must report zero scanned bytes.
No 10 GiB throughput claim is implied merely by supporting that input size.
