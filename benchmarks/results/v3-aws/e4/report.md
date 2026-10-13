# Broker cost (v3-brokers-cost-1)

4 producers, 4 consumers with empty handlers, 64-byte payloads, 30 s per run (5 s warm-up excluded), 5 repetitions; medians across repetitions.

## Peak sustained throughput (as fast as possible)

| Broker | Delivered/s (median) | Range | Published/s | Lost |
|---|---|---|---|---|
| harbinger | 5,293 | 5,120–6,067 | 11,024 | 0 |
| rabbitmq | 7,371 | 6,878–7,775 | 55,882 | 1004319 |
| redis | 21,182 | 20,358–22,000 | 42,070 | 0 |

## Offered 1,000 msg/s

| Broker | Kept up | Delivered/s | P50 ms | P90 ms | P99 ms (range) | P99.9 ms | Max ms | Send lateness P99 ms | Lost |
|---|---|---|---|---|---|---|---|---|---|
| harbinger | 5/5 | 1,000 | 0.244 | 0.425 | 0.623 (0.614–0.675) | 0.827 | 1.1 | 0.179 | 0 |
| rabbitmq | 5/5 | 1,000 | 0.150 | 0.373 | 0.517 (0.483–0.664) | 0.718 | 2.9 | 0.159 | 0 |
| redis | 5/5 | 1,000 | 0.157 | 0.329 | 0.542 (0.342–0.598) | 0.688 | 1.0 | 0.181 | 0 |

## Offered 5,000 msg/s

| Broker | Kept up | Delivered/s | P50 ms | P90 ms | P99 ms (range) | P99.9 ms | Max ms | Send lateness P99 ms | Lost |
|---|---|---|---|---|---|---|---|---|---|
| harbinger | 5/5 | 5,000 | 0.302 | 0.964 | 10.630 (3.662–1226.800) | 28.180 | 29.3 | 0.077 | 0 |
| rabbitmq | 5/5 | 5,000 | 0.116 | 0.138 | 0.307 (0.141–0.359) | 0.366 | 2.8 | 0.072 | 0 |
| redis | 5/5 | 5,000 | 0.096 | 0.194 | 0.314 (0.298–0.337) | 0.416 | 0.7 | 0.072 | 0 |

## Offered 20,000 msg/s

| Broker | Kept up | Delivered/s | P50 ms | P90 ms | P99 ms (range) | P99.9 ms | Max ms | Send lateness P99 ms | Lost |
|---|---|---|---|---|---|---|---|---|---|
| harbinger | 0/5 | 5,219 | 30799.500 | 38512.900 | 40640.800 (39534.100–43089.400) | 40813.000 | 40819.9 | 25124.500 | 0 |
| rabbitmq | 5/5 | 20,000 | 0.234 | 1.338 | 11.357 (3.543–15.950) | 19.160 | 21.2 | 0.077 | 0 |
| redis | 3/5 | 20,015 | 0.225 | 35.020 | 76.692 (46.823–288.769) | 84.295 | 85.4 | 3.546 | 0 |
