#!/bin/bash
# That file here is for reference; actually used the one stored on the host to avoid checkout of the whole code

get_destination() {
  if [ -z "${IS_RELEASE_DIGIT}" ]; then
    IS_RELEASE_DIGIT=$(echo $f | cut -d. -f3 | cut -d- -f1)
    if [[ $((IS_RELEASE_DIGIT % 2)) -eq 0 ]]; then
      DESTINATION="release"
    else
      DESTINATION="dev"
    fi
  fi
}

echo "Collected archives"
ls -1 build/

for f in build/*.zip build/*.exe; do
  packageName="$(basename $f)"
  if [ -f "$f" ]; then
    get_destination
    /usr/bin/docker exec -i repo-generator /generator.sh --path "/repository/manticoresearch_windows/$DESTINATION/x64/" --name "$packageName" --not-index --skip-signing < "$f"
  fi
done

for f in build/*.gz; do
  packageName="$(basename $f)"
  if [ -f "$f" ]; then
    get_destination
    /usr/bin/docker exec -i repo-generator /generator.sh --path "/repository/manticoresearch_macos/$DESTINATION/" --name "$packageName" --not-index --skip-signing < "$f"
  fi
done

rm -rf build/*.{zip,gz,exe}
