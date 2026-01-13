#!/bin/bash
writefile=$1
writestr=$2

if [ $# -eq 2 ]; then
	dirName=$( dirname "$writefile" )
	mkdir -p "$dirName"
	echo "$writestr" > "$writefile"
else
	echo "Invalid number of arguments ($#), expected 2 (writefile writestr)"
	exit 1
fi
