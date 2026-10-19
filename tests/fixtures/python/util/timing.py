"""Timing helpers (fixture)."""
import time


def measure(fn):
    def wrapper(*args, **kwargs):
        start = time.perf_counter()
        result = fn(*args, **kwargs)
        wrapper.last = time.perf_counter() - start
        return result

    return wrapper
