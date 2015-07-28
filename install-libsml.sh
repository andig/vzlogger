#!/bin/sh
set -e
export LIB=libsml
# check to see if folder is empty
if [ ! -d "$HOME/$LIB" ] || [ ! -e "$HOME/$LIB/.git/config" ]; then
  git clone https://github.com/volkszaehler/libsml.git
else
  echo "Using cached directory."
  cd $LIB && git pull && cd ..
fi
cd $LIB
make
ls -R
