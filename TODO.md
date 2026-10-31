# TODO

## Critical

## High

## Medium

- [ ] Decide where per-window view state lives. Options, measurements and a recommendation are in `docs/dev/window-state.md`.

- [ ] `word_wrap` cuts each line at a word boundary and drops the rest. Each file row still takes one screen row (`core.c:867`).

## Low

- [ ] Loopback HTTP proxies fail unless `LOKI_HTTP_ALLOW_INTERNAL` is set: the connect-time check sees the proxy address. Kept as the default (2026-10-25) because exempting the proxy needs curl to report which connection is the proxy. Revisit if someone needs a local proxy.
