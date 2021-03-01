FROM gcc:6.3.0

ADD . /ibench

RUN cd /ibench && make -j

ENV PATH="/ibench/src:${PATH}"

WORKDIR /ibench/src
