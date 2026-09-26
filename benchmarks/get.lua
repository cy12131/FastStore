function done(summary, latency, requests)
    io.write(string.format("FASTSTORE_P50_US=%.0f\n", latency:percentile(50.0)))
    io.write(string.format("FASTSTORE_P95_US=%.0f\n", latency:percentile(95.0)))
    io.write(string.format("FASTSTORE_P99_US=%.0f\n", latency:percentile(99.0)))
    io.write(string.format("FASTSTORE_REQUESTS=%d\n", summary.requests))
end
