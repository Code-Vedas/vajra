---
title: Production Deployment
nav_order: 10
permalink: /production/
---

# Production Deployment

Production deployments should make listener ownership, process supervision,
logging, shutdown, and limits explicit.

## Process Supervision

Run Vajra under a supervisor such as systemd, a container runtime, or a
platform process manager. The supervisor should:

- start the process from the application root
- provide environment variables for port, worker count, and trace endpoints
- restart the process after unexpected exit
- route stdout and stderr to the platform log sink
- send normal termination signals during deploy and shutdown

Example command:

```bash
bundle exec vajra
```

Rails applications continue to use the standard launcher:

```bash
bin/rails server
```

### systemd

Example unit for a Rack application:

```ini
[Unit]
Description=Vajra Rack application
After=network.target

[Service]
Type=simple
WorkingDirectory=/srv/myapp/current
Environment=RACK_ENV=production
Environment=PORT=3000
Environment=WEB_CONCURRENCY=2
ExecStart=/usr/bin/env bundle exec vajra --config config/vajra.rb
Restart=on-failure
KillSignal=SIGTERM
TimeoutStopSec=70

[Install]
WantedBy=multi-user.target
```

Set `TimeoutStopSec` longer than `worker_timeout` so Vajra can drain active
Rack execution before the supervisor sends a hard kill.

## Core Runtime Settings

Set these values deliberately:

```ruby
Vajra.configure do |config|
  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads 5, 5

  config.max_connections 256
  config.socket_queue_capacity 256
  config.max_request_head_bytes 16_384
  config.max_request_body_bytes 104_857_600
  config.request_timeout 25
  config.request_head_timeout 5
  config.first_data_timeout 30
  config.persistent_timeout 30
end
```

Tune `workers` for CPU and memory budget. Tune `threads` for the concurrency
profile of the Rack app. IO-heavy apps can use more threads than CPU-heavy apps.

## TLS

Vajra can terminate TLS at the application server:

```ruby
Vajra.configure do |config|
  config.tls true
  config.tls_certificate "/etc/vajra/server.crt"
  config.tls_private_key "/etc/vajra/server.key"
  config.tls_min_version "TLSv1_2"
end
```

Certificate and key files must be readable by the runtime user. Use
`tls_verify_mode "peer"` only when the deployment requires client certificate
verification and a CA bundle is configured.

## HTTP/2

Enable HTTP/2 deliberately on deployments that need TLS ALPN `h2`, cleartext
h2c, Extended CONNECT, or WebSocket-over-HTTP/2:

```ruby
Vajra.configure do |config|
  config.http2 true
  config.alpn_protocols %w[h2 http/1.1]
end
```

Plain listeners accept h2c prior knowledge and HTTP/1.1 `Upgrade: h2c` when
`http2` is enabled. Extended CONNECT gives applications a bidirectional HTTP/2
stream object while Vajra keeps the shared connection. Applications using
WebSocket-over-HTTP/2 should close or reset accepted streams during their own
shutdown path.

## Logs

Use file-backed access and error logs when the host expects log rotation:

```ruby
Vajra.configure do |config|
  config.access_log "log/vajra-access.log"
  config.access_log_format "json"
  config.error_log "log/vajra-error.log"
  config.structured_logs true
end
```

Send `SIGUSR1` after external log rotation:

```bash
kill -USR1 <vajra-worker-pid>
```

Container platforms can collect stdout and stderr. Use file logs when the host
handles rotation and retention.

## Control Plane

Expose stats and metrics on internal routes:

```ruby
Vajra.configure do |config|
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
end
```

Protect these endpoints at the network or reverse-proxy layer. They expose
runtime process state and operational counters.

Use an app-owned health route, such as `/up`, for load balancer health checks.
Use `stats_path` and `metrics_endpoint` for internal runtime inspection.

## Containers

Build the native extension inside the target Linux image. Do not copy a
host-built extension into a Linux container.

Container checklist:

- install Ruby, Bundler, a C++ compiler, make, and system headers
- run `bundle install` for the application bundle
- compile Vajra in the container image
- set `PORT`, `WEB_CONCURRENCY`, and trace/log environment variables at deploy
- send termination signals during rollout so Vajra can drain

Minimal Dockerfile shape:

```dockerfile
FROM ruby:3.3
WORKDIR /app
COPY Gemfile Gemfile.lock ./
RUN bundle install
COPY . .
ENV RACK_ENV=production
ENV PORT=3000
CMD ["bundle", "exec", "vajra", "--config", "config/vajra.rb"]
```

Build the extension in the image that will run the app.

### Kubernetes

Use the application health route for readiness and liveness:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: myapp
spec:
  replicas: 2
  selector:
    matchLabels:
      app: myapp
  template:
    metadata:
      labels:
        app: myapp
    spec:
      terminationGracePeriodSeconds: 75
      containers:
        - name: web
          image: registry.example.com/myapp:latest
          env:
            - name: PORT
              value: "3000"
            - name: WEB_CONCURRENCY
              value: "2"
          ports:
            - containerPort: 3000
          readinessProbe:
            httpGet:
              path: /up
              port: 3000
          livenessProbe:
            httpGet:
              path: /up
              port: 3000
```

Keep the termination grace period longer than `worker_timeout`.

## Reverse Proxies

Nginx TLS termination:

```nginx
server {
  listen 443 ssl http2;
  server_name example.com;

  ssl_certificate /etc/letsencrypt/live/example.com/fullchain.pem;
  ssl_certificate_key /etc/letsencrypt/live/example.com/privkey.pem;

  location / {
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto https;
    proxy_pass http://127.0.0.1:3000;
  }
}
```

Caddy:

```caddyfile
example.com {
  reverse_proxy 127.0.0.1:3000
}
```

HAProxy:

```haproxy
frontend https
  bind :443 ssl crt /etc/haproxy/certs/example.pem alpn h2,http/1.1
  default_backend vajra

backend vajra
  server app1 127.0.0.1:3000 check
```

If the proxy terminates TLS, keep Vajra on a private listener. If Vajra
terminates TLS directly, configure `tls true`, certificate paths, and ALPN in
`config/vajra.rb`.

## Shutdown

During shutdown, Vajra stops listener admission, drains active Rack execution
within `worker_timeout`, closes idle keep-alive sockets, and releases native
runtime resources. After full hijack, Ruby owns the returned connection object
and must close it. Accepted HTTP/2 tunnels are stream-owned; Vajra resets any
remaining HTTP/2 streams during process shutdown.
