#!/usr/bin/env bash
# from outside: $DISTRO = 7,8
# That file here is for reference; actually used the one stored on the host to avoid checkout of the whole code
set -e
echo "Uploading $DISTRO"
echo "Collected rpm packages"
ls -1 build/

# bundleaarch=0
# bundleintel=0
declare -A archs

for f in build/*.rpm; do

  echo "file $f"
  tail=$(echo "$f" | sed 's_build/__g;s/[a-z]*-//g;')
  if [[ $tail == *".x86_64."* ]]; then
    ARCH=x86_64
  elif [[ $tail == *".aarch64."* ]]; then
    ARCH=aarch64
  fi
  echo "Arch: $ARCH"

  if [ -f "$f" ]; then
    if [ -z "${IS_RELEASE_DIGIT}" ]; then
      IS_RELEASE_DIGIT=$(echo "$f" | cut -d. -f3 | cut -d_ -f1)
      if [[ $((IS_RELEASE_DIGIT % 2)) -eq 0 ]]; then
        DESTINATION="release"
      else
        DESTINATION="dev"
      fi
    fi

    packageName="$(basename "$f")"
    if [[ $ARCH == "x86_64" ]]; then
      /usr/bin/docker exec -i repo-generator /generator.sh -n "$packageName" -d centos -v "$DISTRO" -t "$DESTINATION" -a x86_64 --not-index < "$f"
      archs[x86_64]=1
    fi

    if [[ $ARCH == "aarch64" ]]; then
      /usr/bin/docker exec -i repo-generator /generator.sh -n "$packageName" -d centos -v "$DISTRO" -t "$DESTINATION" -a aarch64 --not-index < "$f"
      archs[aarch64]=1
    fi

  fi
done

# no need to make bundle as we deploy one single package
for arch in "${!archs[@]}"; do
  /usr/bin/docker exec -i repo-generator /generator.sh -d centos -v "$DISTRO" --architecture "$arch" --target "$DESTINATION" --only-index
done


rm -rf build/*.rpm
