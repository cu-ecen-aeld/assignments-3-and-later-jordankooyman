#!/bin/bash
filesdir=$1
searchstr=$2

if [ $# -eq 2 ]; then
	if ! [ -d "$filesdir" ]; then
		echo "Invalid filesdir '$filesdir'"
		exit 1
	fi
	filecount=$( find $filesdir -type f | wc -l )
	searchcount=$( find $filesdir -type f -exec cat {} + | grep "$searchstr" | wc -l )
	echo "The number of files are $filecount and the number of matching lines are $searchcount"
else
	echo "Invalid number of arguments ($#), expected 2 (filesdir searchstr)"
	exit 1
fi
