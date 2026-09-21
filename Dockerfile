# Linux x86-64 build and test environment (docs/notes.md D6). Used by CI; also runs locally with
#   docker build -t rung . && docker run --rm rung
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang cmake ninja-build python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /rung
COPY . .
RUN cmake --preset release && cmake --build --preset release
CMD ["ctest", "--preset", "release"]
