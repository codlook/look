# Elixir counterpart of bench.lk / dbapp.lk: Plug on Bandit, production defaults.
# Without DB_HOST: GET / → {"ok":true,"msg":"hello"}. With DB_HOST: GET / → the row of users id=42.
# Run: MIX_ENV=prod elixir bench.exs   (Mix.install fetches the packages on first run)
Mix.install([{:bandit, "~> 1.5"}, {:jason, "~> 1.4"}, {:myxql, "~> 0.7"}])
if System.get_env("INSTALL_ONLY"), do: System.halt(0)   # used when the image is built

defmodule Bench do
  use Plug.Router
  plug :match
  plug :dispatch

  get "/" do
    body =
      case System.get_env("DB_HOST") do
        nil ->
          Jason.encode!(%{ok: true, msg: "hello"})

        _ ->
          %{rows: rows} = MyXQL.query!(:db, "SELECT id, name, email FROM users WHERE id = ?", [42])
          Jason.encode!(Enum.map(rows, fn [id, name, email] -> %{id: id, name: name, email: email} end))
      end

    conn |> put_resp_content_type("application/json") |> send_resp(200, body)
  end
end

children =
  case System.get_env("DB_HOST") do
    nil -> []
    host ->
      [{MyXQL, name: :db, hostname: host, username: "root", password: System.get_env("DB_PASS"),
        database: "bench", pool_size: 16, socket_options: [nodelay: true]}]
  end

{:ok, _} = Supervisor.start_link(children ++ [{Bandit, plug: Bench, port: 8080}], strategy: :one_for_one)
if System.get_env("DB_HOST") do
  # wait until the pool answers, like the other apps (the database may still be starting)
  Enum.reduce_while(1..60, nil, fn _, _ ->
    case MyXQL.query(:db, "SELECT 1") do
      {:ok, _} -> {:halt, :ok}
      _ -> Process.sleep(500); {:cont, nil}
    end
  end)
end
Process.sleep(:infinity)
