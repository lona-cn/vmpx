#!/bin/bash
xmake f -c -p linux -a x86_64 --toolchain=gcc -k binary -m release --runtimes=c++_static -o build -vy &&\
xmake build -vDya