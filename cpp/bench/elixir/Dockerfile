# docker build -t look-bench-elixir cpp/bench/elixir
FROM elixir:1.16
ENV MIX_ENV=prod
WORKDIR /app
COPY bench.exs .
RUN mix local.hex --force && mix local.rebar --force && INSTALL_ONLY=1 elixir bench.exs
CMD ["elixir", "bench.exs"]
