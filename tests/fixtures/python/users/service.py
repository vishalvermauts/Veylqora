"""User service (fixture)."""


class UserService:
    def __init__(self, store):
        self.store = store

    def create_user(self, name):
        def normalise(value):
            return value.strip().lower()

        user = normalise(name)
        self.store[user] = True
        return user

    def find_user(self, name):
        return self.store.get(name)
