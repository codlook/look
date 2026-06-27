FROM node:22-slim
RUN apt-get update && apt-get install -y default-jre-headless curl && rm -rf /var/lib/apt/lists/*
RUN npm install -g firebase-tools@latest
WORKDIR /app
EXPOSE 8080
