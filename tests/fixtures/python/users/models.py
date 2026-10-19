"""User models (fixture)."""


class User:
    def __init__(self, name):
        self.name = name

    class Profile:
        def describe(self):
            return "profile"

    def display_name(self):
        return self.name.title()
