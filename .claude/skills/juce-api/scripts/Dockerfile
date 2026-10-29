# Doxygen for generating the JUCE API reference from the vendored headers.
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends doxygen \
 && rm -rf /var/lib/apt/lists/*
