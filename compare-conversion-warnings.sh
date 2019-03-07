#!/bin/sh

echo "CFLAGS=-Wconversion" > config.mak

git checkout master
make clean
make > master.txt 2>&1
sed 's/:[0-9]+/:0/g' master.txt > master-mod.txt

git checkout zlib-unsigned-long-removal
make clean
make > feature.txt 2>&1
sed 's/:[0-9]+/:0/g' feature.txt > feature-mod.txt

diff -Nur master-mod.txt feature-mod.txt

exit $?
