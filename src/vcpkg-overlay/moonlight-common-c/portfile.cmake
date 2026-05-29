vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
# Download the xlnt source code
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO moonlight-stream/moonlight-common-c
    REF 2600beaf13f18bfa43453609cf5e3b84a4227760
    SHA512 34392f75c837ff4bc8c3889020784457dddcff95b29124188d2c591b92d2b2580708deb54e9165264d749e9482e173548cf38017e43cc1f391f39f0f949be570
    HEAD_REF master
    )
    
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH_LIBENET
    REPO cgutman/enet
    REF c7353c059373f8d3fc83d451f8f1a477be3dc94e
    SHA512 cbfed6f871367551173f7a5bde2511a6f80679e8fecdfad7c40f30240158a2eb6e7d5707a5dae6ee574083c67a75a4ade83790dd896cefa851876d85fec949dc
    HEAD_REF moonlight
)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH_LIBSIMDE
    REPO simd-everywhere/simde-no-tests
    REF 595b743dcebc05756244a66dcd78e9d64c07b3b7
    SHA512 125642d9c1f7ffd198860b165bbc87ff7805d919600c6cb22652baf3981305d4bdb0cecd25869a189c2bfbaa37957522392d4587f2f793c18a0ac6b5a40c2f5a
    HEAD_REF master
)

file(COPY "${SOURCE_PATH_LIBENET}/" DESTINATION "${SOURCE_PATH}/enet")
file(COPY "${SOURCE_PATH_LIBSIMDE}/" DESTINATION "${SOURCE_PATH}/nanors/deps/simde")

vcpkg_apply_patches(
    SOURCE_PATH ${SOURCE_PATH}
    PATCHES
        0001-add-install-rules.patch
        0002-fix-clang-multiversioning-headers.patch
)

set(BUILD_SHARED_LIBS OFF)  # 强制构建静态库

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=OFF  # 传递给CMake
        -DUSE_MBEDTLS=OFF        # 使用OpenSSL（vcpkg默认）
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME moonlight-common-c)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()  # 修复pkg-config文件

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")