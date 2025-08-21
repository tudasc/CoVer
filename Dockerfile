FROM debian:13

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get -y -qq --no-install-recommends install \
        cmake \
        make \
        zstd \
        git \
        python3 \
        python3-venv \
        unzip \
        nano \
        libzstd-dev \
        wget \
        openmpi-bin \
        libopenmpi-dev \
        openjdk-25-jre openjdk-25-jdk

# Add LLVM Repo
RUN echo "deb https://apt.llvm.org/unstable llvm-toolchain-20 main" \
        > /etc/apt/sources.list.d/llvm.list && \
    wget -qO /etc/apt/trusted.gpg.d/llvm.asc \
        https://apt.llvm.org/llvm-snapshot.gpg.key

RUN apt-get update \
    && apt-get -y -qq --no-install-recommends install \
        clang-20 \
        libclang-rt-20-dev \
        libomp-20-dev \
        clang-format-20 \
        llvm-20 \
        lld-20 \
        llvm-20-dev

RUN ln -s /usr/bin/clang-20 /usr/bin/clang
RUN ln -s /usr/bin/clang++-20 /usr/bin/clang++
RUN ln -s /usr/bin/llvm-link-20 /usr/bin/llvm-link
RUN ln -s /usr/bin/opt-20 /usr/bin/opt
RUN ln -s /usr/bin/llc-20 /usr/bin/llc

ENV CC=clang-20
ENV CXX=clang++-20
ENV OMPI_CC=$CC
ENV OMPI_CXX=$CXX

ENV OMPI_ALLOW_RUN_AS_ROOT=1
ENV OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
ENV PMIX_MCA_gds="hash"
ENV OMPI_MCA_memory="^patcher"

# Install Python dependencies and ensure to activate virtualenv (by setting PATH variable)
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Compile CoVer
COPY ./ /tmp/cover_src
WORKDIR /tmp/cover_src
RUN rm -rf build/ install/
RUN mkdir build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/opt/cover -DCMAKE_BUILD_TYPE=Release && cmake --build . --target install -j

ENV PATH="/opt/cover/bin:$PATH"

WORKDIR /root