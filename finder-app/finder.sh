#!/bin/sh
# Written by Jordan Kooyman
# Modified from BASH version with help from ChatGPT: https://chatgpt.com/share/69742792-4208-8007-a5d3-b6e7b6169269

if [ "$#" -ne 2 ]; then
    echo "Invalid number of arguments ($#), expected 2 (filesdir searchstr)"
    exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d "$filesdir" ]; then
    echo "Invalid filesdir '$filesdir'"
    exit 1
fi

filecount=$(find "$filesdir" -type f | wc -l)

searchcount=$(grep -R "$searchstr" "$filesdir" | wc -l)


echo "The number of files are $filecount and the number of matching lines are $searchcount"

