# Veylqora container image (M3.4).
#
# The daemon binds to 127.0.0.1 inside the container and refuses any other
# address (ADR-0012), so the port is not published to the host. Use the CLI and
# the MCP server from inside the container, for example:
#   docker compose exec veylqora veylqora status --root /workspace --server http://127.0.0.1:8765

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config libsqlite3-dev ca-certificates git \
 && rm -rf /var/lib/apt/lists/*
COPY . /src
# Warnings are not errors in this image build: Ubuntu 24.04's GCC 13 reports a
# false positive in picosha2 that the CI compilers (GCC 15, Clang) do not. The
# warnings-as-errors gate still runs in the gcc and clang CI jobs.
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVEYLQORA_BUILD_TESTS=OFF \
 -DVEYLQORA_WERROR=OFF \
 && cmake --build /build --target veylqora

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsqlite3-0 ca-certificates python3 python3-venv \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --create-home --home-dir /data veylqora
COPY --from=build /build/src/veylqora /usr/local/bin/veylqora
COPY mcp/requirements.txt /opt/veylqora/mcp/requirements.txt
RUN python3 -m venv /opt/venv \
 && /opt/venv/bin/pip install --no-cache-dir -r /opt/veylqora/mcp/requirements.txt
COPY mcp /opt/veylqora/mcp
COPY scripts/e2e_mcp.py /opt/veylqora/scripts/e2e_mcp.py
ENV PATH="/opt/venv/bin:${PATH}" \
    VEYLQORA_URL="http://127.0.0.1:8765"
USER veylqora
WORKDIR /data
VOLUME ["/data"]
EXPOSE 8765
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/api/v1/health', timeout=2)"
CMD ["veylqora", "serve", "--db", "/data/veylqora.db", "--port", "8765"]
