"""Tax tables (fixture)."""

RATES = {"north": 5, "south": 8}


def tax_for(region, cents):
    return cents * RATES.get(region, 0) // 100


class TaxTable:
    def lookup(self, region):
        return RATES.get(region, 0)
