#!/bin/bash
# from outside: $DISTRO = 6,7,8
# That file here is for reference; actually used the one stored on the host to avoid checkout of the whole code

echo "Collected rpm packages"
ls -1 build/

copy_to() {
    echo -e "Copy $1 to /mnt/repo_storage/manticoresearch/$DESTINATION/centos/$DISTRO/$2";
    cp $1 /mnt/repo_storage/manticoresearch/$DESTINATION/centos/$DISTRO/$2 && echo -e "Success"
    echo -e "\n"
}

bundleaarch=0
bundleintel=0

for f in build/*.rpm; do
  echo file $f
  tail=$(echo $f | sed 's_build/__g;s/[a-z]*-//g;')
  VER=$(echo $tail | cut -d. -f1,2,3,4,5)
  if [[ $tail == *".x86_64."* ]]; then
    ARCH=x86_64
  elif [[ $tail == *".aarch64."* ]]; then
    ARCH=aarch64
  fi;
  if [ -f "$f" ]; then
    if [ -z "${IS_RELEASE_DIGIT}" ]; then
    IS_RELEASE_DIGIT=$(echo $f | cut -d. -f3 | cut -d_ -f1)
      if [[ $(($IS_RELEASE_DIGIT % 2)) -eq 0 ]]; then
        DESTINATION="release"
      else
        DESTINATION="dev"
      fi
    fi


    if [[ ! $ARCH == "none" ]]; then
      FILENAME=$(basename "$f")
      cp $f /work/repomanager/docker/rpm_signer/data/$DISTRO/$ARCH/$FILENAME

      /usr/bin/docker exec rpm_signer /worker.sh $DISTRO $ARCH
      if [[ ! $? -eq 0 ]]; then
        echo "Error signing $FILENAME"
        exit 1
      fi

      mv /work/repomanager/docker/rpm_signer/data/$DISTRO/$ARCH/$FILENAME $f

      SIGNATURE=$(rpm -qpi $f 2>/dev/null | grep Signature)
      if [[ $(echo $SIGNATURE | grep "(none)") ]]; then
        echo "Error $FILENAME was not signed"
        exit 1
      fi
    fi

    if [[ $ARCH == "x86_64" ]]; then
      copy_to $f x86_64/
      bundleintel=1
    fi

    if [[ $ARCH == "aarch64" ]]; then
      copy_to $f aarch64/
      bundleaarch=1
    fi

  fi
done

# no need to make bundle as we deploy one single package

if [ "$DESTINATION" = "dev" ]; then
    /usr/bin/docker exec repo-generator /generator.sh -distro centos -version $DISTRO -dev 1
  else
    /usr/bin/docker exec repo-generator /generator.sh -distro centos -version $DISTRO
fi

rm -rf build/*.rpm
