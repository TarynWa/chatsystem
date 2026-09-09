# chatservice 独立构建。
# S0(本文件当前形态为 G0: auth 共享化阶段, 仅 common+auth 两 proto):
#   业务 proto(common/auth/msg/social) 统一 protoc → chatproto;
#   共享底座(base/: db_pool/redis_pool/snowflake) → chatbase_core;
#   每服务实现 → chatauth_core/chatmsg_core/chatsocial_core → server/cli。
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

# ---------- 业务 proto 统一编译(注意 Result 只定义在 common.proto) ----------
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_FILES
  ${PROTO_DIR}/common.proto
  ${PROTO_DIR}/auth.proto
  ${PROTO_DIR}/msg.proto
  ${PROTO_DIR}/social.proto
)
add_custom_command(
  OUTPUT
    ${GEN_DIR}/common.pb.cc ${GEN_DIR}/common.pb.h
    ${GEN_DIR}/auth.pb.cc   ${GEN_DIR}/auth.pb.h
    ${GEN_DIR}/msg.pb.cc    ${GEN_DIR}/msg.pb.h
    ${GEN_DIR}/social.pb.cc ${GEN_DIR}/social.pb.h
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
          --proto_path=${PROTO_DIR} --cpp_out=${GEN_DIR} ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  COMMENT "protoc business protos")
set_source_files_properties(
  ${GEN_DIR}/common.pb.cc ${GEN_DIR}/common.pb.h
  ${GEN_DIR}/auth.pb.cc   ${GEN_DIR}/auth.pb.h
  ${GEN_DIR}/msg.pb.cc    ${GEN_DIR}/msg.pb.h
  ${GEN_DIR}/social.pb.cc ${GEN_DIR}/social.pb.h
  PROPERTIES GENERATED TRUE)

add_library(chatproto STATIC
  ${GEN_DIR}/common.pb.cc
  ${GEN_DIR}/auth.pb.cc
  ${GEN_DIR}/msg.pb.cc
  ${GEN_DIR}/social.pb.cc
)
target_include_directories(chatproto PUBLIC ${GEN_DIR})
target_link_libraries(chatproto PUBLIC protobuf::libprotobuf)

# ---------- 共享底座(自 authservice/dao|cache 迁入 + snowflake) ----------
add_library(chatbase_core STATIC
  base/db_pool.cc base/redis_pool.cc base/snowflake.cc
)
target_include_directories(chatbase_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatbase_core PUBLIC
  PkgConfig::MYSQL PkgConfig::HIREDIS Threads::Threads)

# ---------- auth core(db_pool/redis_pool 已迁到 base, 此处不再编译) ----------
add_library(chatauth_core STATIC
  authservice/authservice_impl.cc
  authservice/dao/user_dao.cc authservice/dao/device_dao.cc
  authservice/dao/session_dao.cc authservice/dao/audit_dao.cc
  authservice/cache/session_cache.cc authservice/cache/fail_counter.cc
  authservice/security/password_util.cc authservice/security/token_util.cc
  authservice/service/user_service.cc authservice/service/session_service.cc
)
target_include_directories(chatauth_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatauth_core PUBLIC chatproto chatbase_core)

# ---------- msg core(S0 单聊) ----------
add_library(chatmsg_core STATIC
  msgservice/msgservice_impl.cc
  msgservice/dao/message_dao.cc msgservice/dao/offline_dao.cc
  msgservice/service/message_service.cc
)
target_include_directories(chatmsg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatmsg_core PUBLIC chatproto chatbase_core)

# ---------- social core(S0 好友+黑名单) ----------
add_library(chatsocial_core STATIC
  socialservice/socialservice_impl.cc
  socialservice/dao/friend_dao.cc socialservice/dao/request_dao.cc
  socialservice/dao/black_dao.cc
  socialservice/service/social_service.cc
)
target_include_directories(chatsocial_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatsocial_core PUBLIC chatproto chatbase_core)

# ---------- 可执行 ----------
function(chat_add_rpc_bin name core main_src)
  add_executable(${name} ${main_src})
  target_include_directories(${name} PRIVATE ${BASE_INC})
  target_link_libraries(${name} PRIVATE ${core}
    ${BASE_LIB}/libmprpc.so ${BASE_LIB}/libnwl.so ${BASE_LIB}/libmuduo_log.so
    protobuf::libprotobuf Threads::Threads)
  target_link_options(${name} PRIVATE -Wl,-rpath,${BASE_LIB})
endfunction()

function(chat_add_cli name main_src)
  add_executable(${name} ${main_src})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${BASE_INC})
  target_link_libraries(${name} PRIVATE chatproto
    ${BASE_LIB}/libmprpc.so protobuf::libprotobuf Threads::Threads)
  target_link_options(${name} PRIVATE -Wl,-rpath,${BASE_LIB})
endfunction()

chat_add_rpc_bin(auth_server chatauth_core authservice/main.cpp)
chat_add_cli(auth_cli authservice/test/auth_cli.cpp)

chat_add_rpc_bin(msg_server chatmsg_core msgservice/main.cpp)
chat_add_cli(msg_cli msgservice/test/msg_cli.cpp)

chat_add_rpc_bin(social_server chatsocial_core socialservice/main.cpp)
chat_add_cli(social_cli socialservice/test/social_cli.cpp)
