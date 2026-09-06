cmake_minimum_required(VERSION 3.14)
project(chat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 所有模块动态库统一输出到项目根目录 lib/
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# 日志模块   -> libmuduo_log.so
add_subdirectory(src/logsystem)

# 网络库     -> libnwl.so（依赖日志模块 lib::muduo_log）
add_subdirectory(src/NetWork)

# 线程池     -> libthreadpool.so
add_subdirectory(src/threadpool)

# RPC       -> libmprpc.so
add_subdirectory(src/mprpc)
