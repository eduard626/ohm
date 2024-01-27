FROM nvidia/cuda:12.1.0-cudnn8-devel-ubuntu20.04

ENV NVIDIA_VISIBLE_DEVICES all
ENV NVIDIA_DRIVER_CAPABILITIES compute,utility,display

ARG DEBIAN_FRONTEND=noninteractive

RUN apt update -y && apt upgrade -y

RUN apt-get install software-properties-common -y

ARG DEBIAN_FRONTEND=noninteractive
RUN add-apt-repository universe &&\
    add-apt-repository multiverse

RUN apt-get update

# Basic dependencies
RUN apt-get -y install \
    sudo \
    clinfo \
    vim \
    wget \
    python3-pip \
    curl \
    build-essential \
    cmake \
    git \
    freeglut3-dev \
    zlib1g-dev \
    libeigen3-dev \
    libglm-dev \
    googletest \
    libtbb-dev \
    libpdal-dev \
    doxygen \
    libglew-dev \
    libglfw3-dev \
    libpng-dev


RUN useradd -m -s /bin/bash -G sudo eduardo

RUN echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> \
    /etc/sudoers

USER eduardo

WORKDIR /home/eduardo


RUN mkdir -p /tmp/ohm
COPY ./ /tmp/ohm/

RUN cd /tmp/ohm && mkdir build && cd build && \
    cmake .. && make -j10 && sudo make install

ENV TERM xterm-256color
RUN echo 'export PS1=[docker]$' > ~/.profile
CMD ["bash", "-l"]
