##!/bin/sh

declare -a packages=("libelf-dev" "libdwarf-dev")

for p in "${packages[@]}"
do
	echo "Installing: " "$p"
	sudo apt-get install $p
done
