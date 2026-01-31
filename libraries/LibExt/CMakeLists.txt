set(ExtSources
    Temp.cpp
)

set(TestDriverSources
    TestDriver.cpp
)

find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    set(CMAKE_C_COMPILER ${HOMEBREW_CLANG})
    set(CMAKE_CXX_COMPILER ${HOMEBREW_CLANGXX})
    message(STATUS "Using Homebrew clang: ${HOMEBREW_CLANG}")
else()
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    message(STATUS "Using system clang (fallback)")
endif()

add_library(Ext STATIC ${ExtSources})

target_include_directories(Ext PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Core/include
)

target_compile_options(Ext PRIVATE -gdwarf-4)
target_compile_options(Ext PRIVATE -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -std=gnu++2b)
target_compile_options(Ext PRIVATE -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -nostdlib)

add_executable(TestDriver ${TestDriverSources})
target_link_libraries(TestDriver Ext)

target_compile_options(TestDriver PRIVATE -gdwarf-4)
target_compile_options(TestDriver PRIVATE -fno-rtti -fno-exceptions -fno-sized-deallocation -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -std=gnu++2b)
