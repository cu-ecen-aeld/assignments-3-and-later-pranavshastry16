#!/bin/sh

# writer.sh <writefile> <writestr>

if [ $# -ne 2 ]; then
    echo "Error: missing required arguments. Command syntax: $0 <writefile> <writestr>"
    exit 1
fi

writefile="$1"
writestr="$2"

writedir=$(dirname "$writefile")

mkdir -p "$writedir" || { echo "Error: could not create directory '$writedir'"; exit 1; }

echo "$writestr" > "$writefile" || { echo "Error: could not write to file '$writefile'"; exit 1; }

