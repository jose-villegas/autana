# Stats: statsi.h and statsf.h

Order statistics of a sample array, such as frame timings or touch positions. [Math index](README.md).

`MATH_DEFINE_STATS` is instantiated as `statsi` for int32 samples and `statsf` for float samples.
Selection is Hoare's FIND:<sup>[[87]](../Citations.md#87)</sup> it partitions the array in place around
the wanted rank, O(n) on average, and returns exactly the value a full sort would put there.

| Function | Result |
|---|---|
| `statsi_percentile(values, count, percent)`, `statsf_percentile(values, count, percent)` | The value at rank $`\min(\lfloor count \cdot percent / 100 \rfloor, count - 1)`$ in ascending order |
| `statsi_median(values, count)`, `statsf_median(values, count)` | The percentile at 50: the value at rank $`\lfloor count / 2 \rfloor`$ |

Both reorder `values` and need `count > 0`. Percent 100 is the largest value.
