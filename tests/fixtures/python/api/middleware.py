"""Request middleware (fixture)."""


class RequestLogger:
    def __init__(self, sink):
        self.sink = sink

    def __call__(self, request):
        self.sink.append(request)
        return request


def log_request(sink, request):
    sink.append(request)
