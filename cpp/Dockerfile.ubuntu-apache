FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    apache2 \
    python3 \
    curl \
    && a2dismod mpm_event \
    && a2enmod mpm_prefork cgi rewrite proxy proxy_fcgi actions \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

CMD ["bash"]
