#!/bin/sh
set -e
export LIB=json-c
# check to see if folder is empty
if [ ! -d "$HOME/$LIB" ] || [ ! -e "$HOME/$LIB/.git/config" ]; then
  git clone https://github.com/json-c/json-c $LIB -b json-c-0.12
else
  echo "Using cached directory."
  cd $LIB && git pull && cd ..
fi
cd $LIB

if [ ! -e Makefile ]; then
	sh autogen.sh
	./configure
fi
make
# sudo make install

ls -R
