"""Token issuing and verification (fixture)."""


def issue_token(user_id):
    return "tok-" + str(user_id)


def verify_token(token):
    return token.startswith("tok-")


class TokenStore:
    def __init__(self):
        self._tokens = {}

    def save(self, user_id, token):
        self._tokens[user_id] = token

    def load(self, user_id):
        return self._tokens.get(user_id)
