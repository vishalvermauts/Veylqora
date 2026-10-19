"""HTTP routes (fixture)."""

ROUTES = []


def register_routes(app):
    for path in ROUTES:
        app.add(path)


def health():
    return {"status": "ok"}
