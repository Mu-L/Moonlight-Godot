vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
# Download the xlnt source code
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO moonlight-stream/moonlight-common-c
    REF master
    SHA512 eeb8e4878b97c43f6d1e7a2837051f18297dfd080957776c13cc4282c0c8e4e7f048fb043c54e66a0a3498ee48b3567a1240c52f7c6bd3ab0c1353ac63e88b05
    HEAD_REF master
    )
    
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH_LIBENET
    REPO cgutman/enet
    REF moonlight
    SHA512 fca8f42585729eb5427c419f23b9c8bea793ec61ebf712a031a7c4572fe1da0f093ffcd085d86f2a0cb306c210d10a5166a987caf0733763b00aabb9ac9a6858
    HEAD_REF moonlight
)

file(COPY "${SOURCE_PATH_LIBENET}/" DESTINATION "${SOURCE_PATH}/enet")

vcpkg_apply_patches(
    SOURCE_PATH ${SOURCE_PATH}
    PATCHES
        0001-add-install-rules.patch
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