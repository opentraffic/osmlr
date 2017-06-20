#!/bin/bash

if [ -z "$*" ]; then
  echo "Usage: $0 Bounding_Box URL Output_Directory Number_of_Processes file_type <Tar_Output>"
  echo "Example Usage: $0 -74.251961,40.512764,-73.755405,40.903125 https://thewebsite.com/dir /data/tiles 10 json|gph|osmlr false"
  echo "NOTE:  Output directory will be deleted and recreated."
  exit 1
fi

catch_exception() {
  if [ $? != 0 ]; then
    echo "[FAILURE] Detected non zero exit status while downloading tiles!"
    exit 1
  fi
}

BBOX=$1
URL=$2
OUTPUT_DIRECTORY=$3
NUMBER_PROCESSES=$4
FILE_TYPE=$5
TAR_OUTPUT=${6:-"false"}

# these have to exist
if [ -z "${BBOX}" ]; then
  echo "[ERROR] Bounding box is not set. Exiting."
  exit 1
fi

if [ -z "${URL}" ]; then
  echo "[ERROR] URL is not set. Exiting."
  exit 1
fi

if [ -z "${NUMBER_PROCESSES}" ]; then
  echo "[ERROR] Number of processes is not set. Exiting."
  exit 1
fi

if [ -z "${FILE_TYPE}" ]; then
  echo "[ERROR] File Type is not set. Exiting."
  exit 1
fi

if [ -z "${OUTPUT_DIRECTORY}" ]; then
  echo "[ERROR] Output Directory is not set. Exiting."
  exit 1
fi

rm -rf ${OUTPUT_DIRECTORY}
mkdir -p ${OUTPUT_DIRECTORY}

echo "[INFO] Building tile list."
./get_tiles.py -b ${BBOX} -s ${FILE_TYPE} > files.txt
catch_exception

echo "[INFO] Downloading tiles."
cat files.txt | xargs -I replace -P ${NUMBER_PROCESSES} curl ${URL}/replace --create-dirs -o ${OUTPUT_DIRECTORY}/replace -f -L &>curl_output.txt

if [ $? != 0 ]; then
  echo ""
  while read f; do
    [ ! -f "${OUTPUT_DIRECTORY}/${f}" ] && echo "[WARN] ${URL}/${f} was not found!"
  done < files.txt
  echo ""
fi

if [ "${TAR_OUTPUT}" == "true" ]; then
  echo "[INFO] Tar'ing tiles."
  stamp=$(date +%Y_%m_%d-%H_%M_%S)
  pushd ${OUTPUT_DIRECTORY} > /dev/null
  find . | sort -n | tar -cf ${OUTPUT_DIRECTORY}/tiles_${stamp}.tar --no-recursion -T -
  catch_exception
fi

echo "[SUCCESS] Download complete, exiting."

