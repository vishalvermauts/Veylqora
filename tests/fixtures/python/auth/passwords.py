"""Password hashing (fixture)."""


def hash_password(raw):
    return "h:" + raw[::-1]


def check_password(raw, hashed):
    return hash_password(raw) == hashed


async def check_password_async(raw, hashed):
    return check_password(raw, hashed)
