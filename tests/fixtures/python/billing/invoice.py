"""Invoices (fixture)."""


def format_money(cents):
    return "$%d.%02d" % (cents // 100, cents % 100)


class Invoice:
    def __init__(self):
        self.lines = []

    def add_line(self, cents):
        self.lines.append(cents)

    def compute_total(self):
        return sum(self.lines)
