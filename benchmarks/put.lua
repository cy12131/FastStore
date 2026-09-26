local next_thread_id = 0

function setup(thread)
    thread:set("thread_id", next_thread_id)
    next_thread_id = next_thread_id + 1
end

function init(args)
    if #args < 2 then
        error("put.lua requires a unique run prefix and payload path")
    end

    run_prefix = args[1]
    local payload = assert(io.open(args[2], "rb"))
    request_body = payload:read("*all")
    payload:close()
    if #request_body ~= 65536 then
        error("PUT payload must be exactly 64 KiB")
    end

    sequence = 0
    request_headers = {
        ["Content-Length"] = tostring(#request_body),
        ["Content-Type"] = "application/octet-stream"
    }
end

function request()
    sequence = sequence + 1
    local filename = string.format(
        "/files/faststore-bench-put-%s-t%d-r%d.bin",
        run_prefix, thread_id, sequence)
    return wrk.format("PUT", filename, request_headers, request_body)
end

function done(summary, latency, requests)
    io.write(string.format("FASTSTORE_P50_US=%.0f\n", latency:percentile(50.0)))
    io.write(string.format("FASTSTORE_P95_US=%.0f\n", latency:percentile(95.0)))
    io.write(string.format("FASTSTORE_P99_US=%.0f\n", latency:percentile(99.0)))
    io.write(string.format("FASTSTORE_REQUESTS=%d\n", summary.requests))
end
