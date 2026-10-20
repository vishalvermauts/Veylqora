from util import helper


class Base:
    def run(self):
        return self.step()

    def step(self):
        return helper(1)


class Child(Base):
    def run(self):
        return super().run()
