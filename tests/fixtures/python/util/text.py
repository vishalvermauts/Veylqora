"""Text helpers (fixture)."""


def slugify(value):
    return "-".join(value.lower().split())


def truncate(value, width):
    return value if len(value) <= width else value[: width - 1] + "..."
