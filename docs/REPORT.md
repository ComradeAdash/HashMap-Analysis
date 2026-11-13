# Findings

The maps were tested on an AMD Ryzen 5 3600 with WSL2 and GCC 11.4 using `-O3`.

## Integer results

These results use 200,000 random keys with capacity reserved before insertion. Times are medians of five runs, in **ns per operation**. Branch miss rates are medians of three hardware profiles. Actual loads ranged from 0.763 to 0.800.

| Map | Insert | Lookup hit | Lookup miss | Branch miss rate |
| --- | ---: | ---: | ---: | ---: |
| Linear probing | 28.32 | 28.48 | 65.75 | 5.21% |
| Chaining | 56.16 | 22.42 | 25.72 | 0.92% |
| std::unordered_map | 72.31 | 33.13 | 36.78 | 0.84% |
| Sparsepp | 68.32 | 24.87 | 33.55 | 4.50% |

## Main observations

- Linear probing had the lowest integer insertion time, while chaining had lower lookup times
- With string keys and Person pointers, chaining's insertion median was 161 ns/op and linear probing's was 175 ns/op, with overlapping timing ranges
- In a separate 2,000-string test, a weak character-sum hash made linear insertion about 40 times slower
- Reserving capacity reduced insertion time in all tested map/type combinations
- Linear insertion was faster with integers even though it had a higher branch miss rate than chaining
- Cachegrind reported fewer D1 misses for linear than chained integer insertion; the order reversed for strings

The results depend on the workload and implementation. Branch and cache counters help explain the behavior, but they do not prove the cause of a timing difference. Cachegrind counts are simulated, and cache misses do not measure memory usage.

The [saved data](data/README.md) includes the full timing and profiling results. See the [testing method](METHODOLOGY.md) for how the runs were set up.
