"""Compare physics_tests output against reference.py numerically.

The C++ harness prints with %.17g and Python with its shortest round-trip repr,
so a plain text diff spuriously fails even when the doubles are identical. This
compares value-by-value with a tolerance instead. Exit code 1 on any mismatch.

Usage: python compare.py <got.txt> <ref.txt>
"""
import sys

TOL = 1e-9


def load(path):
    data = {}
    for line in open(path):
        parts = line.strip().split(",")
        if not parts or not parts[0]:
            continue
        data[parts[0]] = [float(x) for x in parts[1:]]
    return data


def main():
    got = load(sys.argv[1])
    ref = load(sys.argv[2])
    failed = False

    for key, expected in ref.items():
        if key not in got:
            print(f"MISSING in C++ output: {key}")
            failed = True
            continue
        for i, (a, b) in enumerate(zip(got[key], expected)):
            if abs(a - b) > TOL:
                print(f"DIFF {key}[{i}]: got {a!r} ref {b!r} (|d|={abs(a-b):.2e})")
                failed = True

    # Renderer sanity: the centre pixel must be the opaque red pivot.
    center = got.get("render_center_BGRA")
    if center != [40.0, 40.0, 230.0, 255.0]:
        print(f"render centre pixel unexpected: {center}")
        failed = True

    print("RESULT:", "FAIL" if failed else "OK -- C++ matches the Python reference")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
