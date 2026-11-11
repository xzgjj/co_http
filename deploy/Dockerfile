# co_http 协程版镜像（多阶段：gcc:13 构建 → debian 运行）
FROM gcc:13 AS build
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j --target chat_server_co

FROM debian:bookworm-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/chat_server_co /app/chat_server
COPY --from=build /src/static /app/static
ENV PORT=8080 STATIC_ROOT=/app/static
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD ["/bin/sh", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080; printf 'GET /healthz HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n' >&3; head -n 1 <&3 | grep -q ' 200 '"]
CMD ["/app/chat_server"]
