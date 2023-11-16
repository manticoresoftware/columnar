#!/bin/bash

# To be run from gitlab CI runner.

autotag() {
  API_VER=$(grep -Po 'LIB_VERSION.* \K\d+' columnar/columnar.h)
  SI_API_VER=$(grep -Po 'LIB_VERSION.* \K\d+' secondary/secondary.h)
  KNN_API_VER=$(grep -Po 'LIB_VERSION.* \K\d+' knn/knn.h)
  AUTO_TAG="c$API_VER-s$SI_API_VER-k$KNN_API_VER"
  NUMS=3

  # check whether all the numbers are available
  if [[ $(echo $AUTO_TAG | grep -Po '[0-9]+' | wc -l) = $NUMS ]]; then
      # tag is correct
      if [[ ! $(git ls-remote github | grep $AUTO_TAG) ]]; then
          echo "no tag - will add $AUTO_TAG"
          echo "> git tag $AUTO_TAG"
          git tag "$AUTO_TAG"
          echo "> git push github $AUTO_TAG"
          git push github "$AUTO_TAG"
          echo "> git status"
          git status
      else
          echo "repo github already has tag $AUTO_TAG, exiting..."
      fi
  else
      echo "generated tag $AUTO_TAG is not valid, do nothing"
  fi
}

echo "> rm -fr gitlab_github_sync"
rm -fr gitlab_github_sync
echo "> git clone git@gitlab.com:manticoresearch/columnar.git gitlab_github_sync"
git clone git@gitlab.com:manticoresearch/columnar.git gitlab_github_sync
echo "> cd gitlab_github_sync"
cd gitlab_github_sync
echo "> git checkout $CI_COMMIT_BRANCH"
git checkout $CI_COMMIT_BRANCH
echo "> git remote add github git@github.com:manticoresoftware/columnar.git"
git remote add github git@github.com:manticoresoftware/columnar.git
echo "> git fetch github"
git fetch github
echo "> git push -u github $CI_COMMIT_BRANCH"
git push -u github "$CI_COMMIT_BRANCH"
if [[ $CI_COMMIT_BRANCH == "master" ]]; then autotag; fi
