# Vajra Performance

Small local benchmark project for comparing Vajra against peer Rack servers.

It generates Rack, Rails, Roda, Sinatra, and Hanami fixtures under `tmp/<timestamp>/`,
starts each fixture through each selected server, runs k6 against the already-running
server, and reports:

- requests per second
- requests per CPU core
- request count
- error rate
- p95 and p99 latency
- RSS min, max, and final bytes for the server process group

## Run

```bash
cd gems/vajra/performance
bundle exec rake performance:run
```

Local knobs are ENV variables:

```bash
SERVERS=vajra,puma WORKER_COUNT=1 THREAD_COUNT=4 bundle exec rake performance:run
```

Access logging is off by default. Set `ACCESS_LOG=1` to enable fair access
logging for every selected server.

Set `SOAK_SECONDS=300` to run the same matrix as a practical local soak. The
default run duration is 20 seconds per server/fixture pair.

Each generated fixture uses the same request mix:

- `GET /text`
- `GET /json`
- `POST /json`
- `POST /form` with text fields and a file
- `POST /upload` with a file

Generated apps, k6 scripts, summaries, and logs are written under
`tmp/<timestamp>/<fixture>/`. Each server/fixture pair writes
`<server>.run-summary.json`, and the whole run writes `summary.json` at the
timestamp root. The comparison table prints to stdout.
