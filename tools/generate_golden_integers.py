#!/usr/bin/env python3
"""Generate the integer rows of the golden corpus from the reference.

Each row records a configuration, a count, and the sketch the reference
produces when the integers zero up to that count are added to a sketch
built for integers. The corpus test rebuilds each row through this
library and compares, so the integer hash is covered by a build that
does not run the reference at all.

Usage:
    tools/generate_golden_integers.py <classpath> [>> tests/golden_corpus.tsv]

The classpath must contain the reference library and the compiled
tests/ZetaSketchCli.java driver.
"""

import base64
import subprocess
import sys

CONFIGURATIONS = [
    (4, 0), (4, 4), (4, 9), (6, 11), (9, 14), (10, 15), (10, 20),
    (10, 24), (14, 15), (14, 20), (14, 24), (15, 15), (20, 20), (20, 24),
]
POPULATIONS = [0, 1, 10, 100, 1000, 10000, 100000]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    classpath = sys.argv[1]

    blocks = []
    for precision, sparse_precision in CONFIGURATIONS:
        for population in POPULATIONS:
            blocks.append(
                f"SKETCH {precision} {sparse_precision} longs\n"
                + "".join(f"LONG {value}\n" for value in range(population))
            )

    completed = subprocess.run(
        ["java", "-cp", classpath, "ZetaSketchCli", "CREATE_BATCH", "4", "0"],
        input="".join(blocks),
        capture_output=True,
        text=True,
        check=True,
    )
    sketches = completed.stdout.strip().splitlines()

    # The estimate each sketch reports, so that a row pins what the
    # reference counts as well as what it writes.
    estimated = subprocess.run(
        ["java", "-cp", classpath, "ZetaSketchCli", "TRANSITION", "4", "0"],
        input="".join(f"RESULT\t{sketch}\t\n" for sketch in sketches),
        capture_output=True,
        text=True,
        check=True,
    )
    cardinalities = [
        line.split("\t")[1] for line in estimated.stdout.strip().splitlines()
    ]
    if len(cardinalities) != len(sketches):
        print("estimate count does not match sketch count", file=sys.stderr)
        return 1
    expected = len(CONFIGURATIONS) * len(POPULATIONS)
    if len(sketches) != expected:
        print(f"expected {expected} sketches, got {len(sketches)}", file=sys.stderr)
        return 1

    index = 0
    for precision, sparse_precision in CONFIGURATIONS:
        for population in POPULATIONS:
            encoded = base64.b64encode(bytes.fromhex(sketches[index])).decode()
            index += 1
            name = f"LONGS_P{precision}_SP{sparse_precision}_POP{population}"
            print(
                f"LONGS\t{name}\t{precision}\t{sparse_precision}\t{population}"
                f"\t{encoded}\t{cardinalities[index - 1]}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
