# docker build -t look-ws-go -f cpp/bench/ws/Dockerfile.go cpp/bench/ws
# The Go echo server (/server) and the load client (/client), both on gorilla/websocket.
FROM golang:1.22 AS build
WORKDIR /src
COPY server.go client.go ./
RUN go mod init wsbench && go get github.com/gorilla/websocket@v1.5.3 \
 && mkdir -p s c && mv server.go s/ && mv client.go c/ \
 && CGO_ENABLED=0 go build -o /server ./s && CGO_ENABLED=0 go build -o /client ./c
FROM debian:bookworm-slim
COPY --from=build /server /client /
CMD ["/server"]
