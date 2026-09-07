# chatservice(AuthService) 独立构建(S0):
#   编译 auth.proto → chatauth_proto; 内部实现 → chatauth_core; 两个可执行(auth_server/auth_cli)
# 底座 .so 直连 ../../lib(其内含绝对 RPATH, 运行时自解析 nwl/muduo_log)。
cmake_minimum_required(VERSION 3.14)
project(chatservice CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# 产物就近输出, 便于 "cd src/chatservice && ./bin/auth_server -i conf/auth.conf"
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(BASE_LIB ${ROOT}/lib)
set(BASE_INC
    ${ROOT}/src/mprpc/include
    ${ROOT}/src/NetWork/include
    ${ROOT}/src/logsystem/logsys/include
)

# ---------- 第三方 ----------
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# ---------- 业务 proto 编译(auth.pb.{h,cc}) ----------
set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/proto/auth.proto)
set(GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
add_custom_command(
  OUTPUT  ${GEN_DIR}/auth.pb.cc ${GEN_DIR}/auth.pb.h
  COMMAND ${Protobuf_PROTOC_EXECUTABLE} --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto --cpp_out=${GEN_DIR} ${PROTO_SRC}
  DEPENDS ${PROTO_SRC}
  COMMENT "protoc auth.proto -> auth.pb.{h,cc}")
set_source_files_properties(${GEN_DIR}/auth.pb.cc ${GEN_DIR}/auth.pb.h
  PROPERTIES GENERATED TRUE)
add_library(chatauth_proto STATIC ${GEN_DIR}/auth.pb.cc)
target_include_directories(chatauth_proto PUBLIC ${GEN_DIR})
target_link_libraries(chatauth_proto PUBLIC protobuf::libprotobuf)

# ---------- 内部实现(authservice/* + common 头) ----------
set(CORE_SRC
  authservice/authservice_impl.cc
  authservice/dao/db_pool.cc
  authservice/dao/user_dao.cc
  authservice/dao/device_dao.cc
  authservice/dao/session_dao.cc
  authservice/dao/audit_dao.cc
  authservice/cache/redis_pool.cc
  authservice/cache/session_cache.cc
  authservice/cache/fail_counter.cc
  authservice/security/password_util.cc
  authservice/security/token_util.cc
  authservice/service/user_service.cc
  authservice/service/session_service.cc
)
add_library(chatauth_core STATIC ${CORE_SRC})
# 代码内以仓库根相对 include: "common/.." / "authservice/.."
target_include_directories(chatauth_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatauth_core PUBLIC chatauth_proto
  PkgConfig::MYSQL PkgConfig::HIREDIS Threads::Threads)

# ---------- 可执行 ----------
# rpath: 让可执行文件能定位 libmprpc.so(mprpc 自身 rpath 已指回 BASE_LIB)
add_executable(auth_server authservice/main.cpp)
target_include_directories(auth_server PRIVATE ${BASE_INC})
target_link_libraries(auth_server PRIVATE chatauth_core
  ${BASE_LIB}/libmprpc.so ${BASE_LIB}/libnwl.so ${BASE_LIB}/libmuduo_log.so
  protobuf::libprotobuf Threads::Threads)
target_link_options(auth_server PRIVATE -Wl,-rpath,${BASE_LIB})

add_executable(auth_cli authservice/test/auth_cli.cpp)
target_include_directories(auth_cli PRIVATE ${BASE_INC})
target_link_libraries(auth_cli PRIVATE chatauth_proto
  ${BASE_LIB}/libmprpc.so protobuf::libprotobuf Threads::Threads)
target_link_options(auth_cli PRIVATE -Wl,-rpath,${BASE_LIB})
