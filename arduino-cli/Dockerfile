#FROM multiarch/crossbuild
FROM golang:1.14

ENV PATH $PATH:/usr/local/go/bin

# Install tooling
ENV GORELEASER_VER 0.113.0
ENV GORELEASER_SHA 2379beebb6369b75ccead7f7a43269de700b51821feae3857701d106ed72bd63
ENV GOVER 1.13
RUN set -ex \
    && curl -o goreleaser.tar.gz -LO https://github.com/goreleaser/goreleaser/releases/download/v${GORELEASER_VER}/goreleaser_Linux_x86_64.tar.gz \
    && echo "$GORELEASER_SHA goreleaser.tar.gz" | sha256sum -c - || exit 1 \
    && tar -xzf goreleaser.tar.gz -C /usr/bin/ goreleaser \
    && rm goreleaser.tar.gz \
    && curl -LO https://dl.google.com/go/go${GOVER}.linux-amd64.tar.gz \
    && tar -C /usr/local -xzf go${GOVER}.linux-amd64.tar.gz \
    && rm go${GOVER}.linux-amd64.tar.gz

RUN curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
ENV PATH=$PATH:/workdir/bin

COPY arduino-cli.yaml /root/.arduino15/arduino-cli.yaml
COPY cli-config.yml /root/.arduino15/.cli-config.yml

RUN mkdir -p /root/Arduino/libraries/ && cd /root/Arduino/libraries/ && git clone https://github.com/e-radionicacom/Inkplate-6-Arduino-library.git && git clone https://github.com/e-radionicacom/Inkplate-6-SDFat-Arduino-Library.git

RUN arduino-cli core update-index

RUN git clone https://github.com/leopck/microbrowser.git

RUN arduino-cli core install Croduino_Boards:esp32
#Show all board FQBN
#RUN arduino-cli board listall
#root@4775c80c6607:/workdir/microbrowser# arduino-cli compile -b Croduino_Boards:esp32:Inkplate6 --libraries="/workdir/Inkplate-6-Arduino-library" microbrowser.ino

RUN apt-get update && apt-get install -y python-pip && pip install pyserial
WORKDIR /go/microbrowser
#COPY Makefile /go/microbrowser/Makefile
#RUN arduino-cli compile --fqbn Croduino_Boards:esp32:Inkplate6 microbrowser.ino
CMD ["make"]
