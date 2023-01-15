#!/bin/bash
# from outside: $DISTRO = jessie,stretch,buster,trusty,xenial,bionic,focal,jammy
# That file here is for reference; actually used the one stored on the host to avoid checkout of the whole code
set -e
echo "Uploading $DISTRO"
echo "Collected debian packages in build/"
ls -1 build/

declare -A archs

for f in build/*deb; do

  packageName="$(basename "$f")"

  ARCH=$(echo "$f" | cut -d_ -f3 | cut -d. -f1)
  echo "Arch: $ARCH"

  if [ -f "$f" ]; then
    if [ -z "$IS_RELEASE_DIGIT" ]; then
      IS_RELEASE_DIGIT=$(echo "$f" | cut -d. -f3 | cut -d- -f1)
      if [[ $((IS_RELEASE_DIGIT % 2)) -ne 0 ]]; then
          TARGET="dev"
        else
          TARGET="release"
      fi
    fi

    if [[ $ARCH == "amd64" ]]; then
      archs[amd]=1
      /usr/bin/docker exec -i repo-generator /generator.sh --distro "$DISTRO" --architecture amd --target "$TARGET" --name "$packageName" --not-index < "$f"
    fi

    if [[ $ARCH == "arm64" ]]; then
      archs[arm]=1
      /usr/bin/docker exec -i repo-generator /generator.sh --distro "$DISTRO" --architecture arm --target "$TARGET" --name "$packageName" --not-index < "$f"
    fi

    if [[ $ARCH == "all" ]]; then
      /usr/bin/docker exec -i repo-generator /generator.sh --distro "$DISTRO" --architecture amd --target "$TARGET" --name "$packageName" --not-index < "$f"
      /usr/bin/docker exec -i repo-generator /generator.sh --distro "$DISTRO" --architecture arm --target "$TARGET" --name "$packageName" --not-index < "$f"
      archs[amd]=1
      archs[arm]=1
    fi
  fi
done


for arch in "${!archs[@]}"; do
  /usr/bin/docker exec -i repo-generator /generator.sh --distro "$DISTRO" --architecture "$arch" --target "$TARGET" --only-index
done

rm -rf build/*deb
