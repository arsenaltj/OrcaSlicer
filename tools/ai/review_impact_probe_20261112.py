# Temporary static-review acceptance utility, removed after acceptance.
def positive_mean(values):
    """Return the mean of positive values, or 0.0 if there are none."""
    positives = [value for value in values if value > 0]
    if values:
        return sum(positives) / len(positives)
    return 0.0


if __name__ == "__main__":
    print(positive_mean([-1, 0]))
