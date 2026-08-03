# ZetaSketch C++

[![Build and Test][build-badge]][build-link]
[![Static Analysis][static-badge]][static-link]
[![Sanitizers][sanitizer-badge]][sanitizer-link]
[![Differential Fuzzing][fuzzing-badge]][fuzzing-link]
[![Code Coverage][coverage-badge]][coverage-link]
[![codecov][codecov-badge]][codecov-link]
[![arm64][arm64-badge]][arm64-link]

The purpose of this repository is to provide a standalone, modern, and rigorous
C++ implementation of Google's ZetaSketch format, which is the foundational
serialization schema used for HyperLogLog++ aggregate mutations within Cloud
Bigtable and BigQuery.

It is a well-established fact that Google relies extensively upon C++ internally
for these operations; however, the open-source community has hitherto been
provided only with a Java implementation. The absence of a native C++ library
has compelled developers into awkward compromises. This project remedies that
deficiency by providing an implementation that is entirely written in safe,
modern, and performant C++23.

## Architectural Design and Precision

The primary objective of this project is to achieve precise byte-identical
serialization with the original Java implementation. Any discrepancy in the
serialized byte array, however minor, would render the output incompatible with
the existing Cloud Bigtable ecosystem. To ensure this strict conformity, our
architectural strategy avoids unnecessary reinvention.

Instead of attempting to reverse-engineer the serialization logic, we rely upon
direct integration. We have incorporated the exact Protocol Buffer definitions
(`hllplusplus.proto`) directly from Google's repository. We compile these
definitions using the `protoc` compiler, thereby ensuring that the structural
encoding remains accurate. Furthermore, the hashing algorithm is not merely an
imitation; we have copied the exact `farmhash.cc` and `farmhash.h` source files
from Google's FarmHash repository. This guarantees that the `Fingerprint64`
hashing operations produce results identical to the original implementation.

For the internal state machine, which governs the complex transitions between
sparse and dense representations, we have executed an idiomatic translation of
the `zetasketch-rs` Rust codebase. The Rust language, with its emphasis on
deterministic memory management and strict typological control, provided a
superior template for translation into modern C++ than the original,
inheritance-heavy Java architecture.

## Performance and Code Hygiene

We have designed this library with a strict adherence to the performance
constraints required by RowKeyDB. Most notably, there is zero memory allocation
on the hot path. We avoid dynamic allocation during active sketch mutations,
relying instead upon pre-allocated, fixed-capacity arrays managed exclusively
through Resource Acquisition Is Initialization (RAII).

The repository maintains a high standard of code hygiene. Our continuous
integration pipeline executes hermetic builds within Docker containers, ensuring
determinism across all environments. The code is subjected to rigorous cross-
translation-unit (CTU) static analysis using `clang-tidy`, and it is
continuously monitored by an array of runtime sanitizers (Address, Memory,
Thread, and Undefined Behavior).

## The Verification Regimen

To justify immense confidence in the correctness of this implementation, we
subject the codebase to a thorough verification regimen. We do not rely upon
theoretical assumptions. Instead, we utilize the original Java library to
generate a large corpus of binary Protocol Buffer "golden files," representing a
multitude of distinct execution states and merge combinations.

Our testing apparatus ingests hours of randomized input strings, processes them
through our C++ architecture, and subjects the resulting serialized byte arrays
to a strict, byte-for-byte comparison against the Java output. We test the
explicit boundaries of the sparse-to-dense transition, the enforcement of
Protocol Buffers' explicit-set semantics, and the behavior of the data
structures under heavy loads via differential fuzzing. The result is verifiable
confirmation that our output is identical to the original.

## License and Copyright

This project is licensed under the Apache License, Version 2.0 (APL). The code
that has been copied directly from Google (such as the FarmHash implementation
and the Protocol Buffer definitions) retains its original licensing and
copyright notices. The logic that has been ported from the `zetasketch-rs`
project includes all necessary credits and attributions to its original authors.
All other modifications and original code within this repository are Copyright
RowKeyDB (2026).

[build-badge]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/build-test.yml/badge.svg
[build-link]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/build-test.yml
[static-badge]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/static_analysis.yml/badge.svg
[static-link]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/static_analysis.yml
[sanitizer-badge]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/sanitizers.yml/badge.svg
[sanitizer-link]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/sanitizers.yml
[fuzzing-badge]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/fuzzing.yml/badge.svg
[fuzzing-link]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/fuzzing.yml
[coverage-badge]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/coverage.yml/badge.svg
[coverage-link]: https://github.com/rowkeydb-com/zetasketch-cpp/actions/workflows/coverage.yml
[codecov-badge]: https://codecov.io/gh/rowkeydb-com/zetasketch-cpp/graph/badge.svg
[codecov-link]: https://codecov.io/gh/rowkeydb-com/zetasketch-cpp
[arm64-badge]: https://img.shields.io/badge/arm64-supported-brightgreen.svg
[arm64-link]: https://github.com/rowkeydb-com/zetasketch-cpp
