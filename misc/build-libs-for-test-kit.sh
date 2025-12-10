# Handle --clean parameter
if [ "$1" = "--clean" ]; then
	rm -rf ../build
fi

mkdir -p ../build
docker run -it --rm --platform linux/amd64 \
-v $(pwd)/../cache:/cache \
-e CACHEB="../cache" \
-e VERBOSE=1 \
-e DIAGNOSTIC=1 \
-e NO_TESTS=1 \
-e DISTR=jammy \
-e boost=boost_nov22 \
-e sysroot=roots_nov22 \
-e arch=x86_64 \
-e CTEST_CMAKE_GENERATOR=Ninja \
-e CTEST_CONFIGURATION_TYPE=RelWithDebInfo \
-e SYSROOT_URL="https://repo.manticoresearch.com/repository/sysroots" \
-e HOMEBREW_PREFIX="" \
-e PACKAGE_VERSION="" \
-e RPM_PACKAGE_VERSION="" \
-v $(pwd)/../../manticore_github:/manticore \
-v $(pwd)/../:/columnar \
manticoresearch/external_toolchain:20251210 bash -c 'echo "SOURCE_DIR /manticore" > /columnar/local_manticore_src.txt && export CXX=clang++ && export CC=clang && cd /columnar/build && cmake -DDISTR=jammy -DBUILD_EMBEDDINGS_LOCALLY=ON .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build .'
