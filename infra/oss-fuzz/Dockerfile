# OSS-Fuzz build image for LOOK.
# Copy this file (with project.yaml and build.sh) to google/oss-fuzz under
# projects/look/ when submitting the integration PR.
FROM gcr.io/oss-fuzz-base/base-builder
RUN git clone --depth 1 https://github.com/codlook/look.git look
WORKDIR look
COPY build.sh $SRC/
