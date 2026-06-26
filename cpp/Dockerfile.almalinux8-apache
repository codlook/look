FROM almalinux:8

RUN dnf install -y \
    httpd \
    python3 \
    curl \
    && dnf clean all

RUN echo "ServerName localhost" >> /etc/httpd/conf/httpd.conf

WORKDIR /src

CMD ["bash"]
