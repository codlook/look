# docker build -t look-bench-go cpp/bench/go
FROM golang:1.22 AS build
WORKDIR /src
COPY main.go .
RUN go mod init bench && go get github.com/go-sql-driver/mysql@v1.8.1 && CGO_ENABLED=0 go build -o /bench .
FROM debian:bookworm-slim
COPY --from=build /bench /bench
CMD ["/bench"]
