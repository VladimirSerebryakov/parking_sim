FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev cmake make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY part2 ./part2
COPY part3 ./part3
COPY part4 ./part4
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build /build --target parking_tcp -j 2

FROM debian:bookworm-slim
WORKDIR /opt/parking
COPY --from=builder /build/bin/part4/parking_server /build/bin/part4/parking_car \
    /build/bin/part4/parking_arrivals /build/bin/part4/parking_monitor ./bin/
RUN mkdir /results
ENV PATH="/opt/parking/bin:${PATH}"
EXPOSE 5050
CMD ["parking_server", "0.0.0.0", "5050", "-L", "/results/parking_result.txt"]
