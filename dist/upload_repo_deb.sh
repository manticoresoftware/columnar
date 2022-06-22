#!/bin/bash
# from outside: $DISTRO = jessie,stretch,buster,trusty,xenial,bionic,focal,jammy
# That file here is for reference; actually used the one stored on the host to avoid checkout of the whole code

echo "Collected debian packages in build/"
ls -1 build/

copy_to() {
  echo -e "Copy $1 to /mnt/repo_storage/manticoresearch_$DISTRO$SUFFIX/dists/$2";
  cp $1 /mnt/repo_storage/manticoresearch_$DISTRO$SUFFIX/dists/$2 && echo -e "Success"
  echo
}

bundleamd=0
bundlearm=0

for f in build/*deb; do
  VER=$(echo $f | cut -d_ -f2)
  ARCH=$(echo $f | cut -d_ -f3 | cut -d. -f1)
  if [ -f "$f" ]; then
    if [ -z "${IS_RELEASE_DIGIT}" ]; then
      IS_RELEASE_DIGIT=$(echo $f | cut -d. -f3 | cut -d- -f1)
      if [[ $(($IS_RELEASE_DIGIT % 2)) -ne 0 ]]; then
        SUFFIX="_dev"
      fi
    fi

    ~/sign_deb.sh $GPG_SECRET $f

    if [[ $ARCH == "amd64" ]]; then
      copy_to $f $DISTRO/main/binary-amd64/
      bundleamd=1
    fi

    if [[ $ARCH == "arm64" ]]; then
      copy_to $f $DISTRO/main/binary-arm64/
      bundlearm=1
    fi
  fi
done

# no need to make bundle as we deploy one single package

if [ ! -z $SUFFIX ]; then
  /usr/bin/docker exec repo-generator /generator.sh -distro $DISTRO -architecture amd -dev 1
  /usr/bin/docker exec repo-generator /generator.sh -distro $DISTRO -architecture arm -dev 1
else
  /usr/bin/docker exec repo-generator /generator.sh -distro $DISTRO -architecture amd
  /usr/bin/docker exec repo-generator /generator.sh -distro $DISTRO -architecture arm
fi

rm -rf build/*deb
