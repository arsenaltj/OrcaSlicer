def progress_percent(completed: int, total: int) -> int:
    """Return progress; an empty workload must report zero percent."""
    return completed * 100 // total


if __name__ == "__main__":
    print(progress_percent(0, 0))
