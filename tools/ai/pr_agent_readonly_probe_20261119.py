"""Standalone command: empty workloads must print zero percent."""
import argparse


def progress_percent(completed: int, total: int) -> int:
    return completed * 100 // total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--completed", type=int, default=0)
    parser.add_argument("--total", type=int, default=0)
    args = parser.parse_args()
    print(progress_percent(args.completed, args.total))


if __name__ == "__main__":
    main()
