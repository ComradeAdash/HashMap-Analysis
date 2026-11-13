# Testing method

- All maps receive the same keys, values, and hash function
- Inputs are sequential, shuffled, or random, with unique keys and a fixed seed
- The two datasets are `uint64_t → uint64_t` and `string → Person*`
- The dataset owns the Person objects; maps store non-owning pointers
- Reserved runs allocate capacity before timing; growing runs include resizing during insertion
- Input generation, lookup setup, validation, and cleanup are outside the timed region
- The runner discards one warmup round and reports medians from fresh processes
- Successful lookups use inserted keys; unsuccessful lookups use a separate set of absent keys
- Hardware counters and Cachegrind simulations measure the operation region separately

The saved runs use seed 42 and a requested maximum load of 0.8. Actual loads differ because each map chooses its capacity differently. Successful Person-pointer lookups also read the person's age.

These are measurements from one machine. CPU scheduling, allocation, and cache state can affect the results. A D1 miss can be served by another cache level, so it does not necessarily mean a RAM access.
