# Nomi（nomi/）：main/CMakeLists.txt 在选好板子之后 include 进来，和它同一个作用域（往 SOURCES、INCLUDE_DIRS 里加）。
#
# 按 Kconfig（nomi/Kconfig）的开关把各能力的文件夹编进来：core 一定有，其他能力关掉就整个文件夹不编。
# 源文件是 glob 的：新加、删了 .c/.cc 要 idf.py reconfigure。
if(CONFIG_NOMI)
    set(NOMI_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(NOMI_ALL_GROUPS core sdcard apps player reader photo camera phone hotspot)

    set(_nomi_groups core)
    foreach(_nomi_cap SDCARD APPS PLAYER READER PHOTO CAMERA PHONE HOTSPOT)
        if(CONFIG_NOMI_${_nomi_cap})
            string(TOLOWER ${_nomi_cap} _nomi_dir)
            list(APPEND _nomi_groups ${_nomi_dir})
        endif()
    endforeach()
    message(STATUS "Nomi capabilities: ${_nomi_groups}")
    foreach(_nomi_dir ${_nomi_groups})
        file(GLOB _nomi_src ${NOMI_DIR}/${_nomi_dir}/*.c ${NOMI_DIR}/${_nomi_dir}/*.cc)
        list(APPEND SOURCES ${_nomi_src})
    endforeach()
    # 头文件目录全加上：关掉的能力只是不编它的源文件，用到它的地方由 CONFIG_NOMI_* 包着
    foreach(_nomi_dir ${NOMI_ALL_GROUPS})
        list(APPEND INCLUDE_DIRS ${NOMI_DIR}/${_nomi_dir})
    endforeach()

    # 打进资源分区的文件（nomi/preinstall/）：按开着的能力挑出来放进编译目录，再交给资源打包。
    # 预装应用包开机装进机器内部的应用分区（nomi/apps/nomi_pkg.cc）；GBK 对照表给阅读器（nomi/reader/nomi_book.cc）
    set(_nomi_pre_files "")
    if(CONFIG_NOMI_READER)
        list(APPEND _nomi_pre_files gbk.tab nomi.novel.npk)
    endif()
    if(CONFIG_NOMI_PLAYER)
        list(APPEND _nomi_pre_files nomi.music.npk)
    endif()
    if(CONFIG_NOMI_PHOTO)
        list(APPEND _nomi_pre_files nomi.photo.npk)
    endif()
    set(_nomi_stage ${CMAKE_BINARY_DIR}/nomi_preinstall)
    # 只删这次不要的：留着的文件内容没变就不重写（configure_file 不动它），资源分区不会白重打
    file(GLOB _nomi_staged ${_nomi_stage}/*)
    foreach(_nomi_old ${_nomi_staged})
        get_filename_component(_nomi_name ${_nomi_old} NAME)
        if(NOT _nomi_name IN_LIST _nomi_pre_files)
            file(REMOVE ${_nomi_old})
        endif()
    endforeach()
    foreach(_nomi_file ${_nomi_pre_files})
        configure_file(${NOMI_DIR}/preinstall/${_nomi_file} ${_nomi_stage}/${_nomi_file} COPYONLY)
    endforeach()
    if(_nomi_pre_files AND NOT DEFAULT_ASSETS_EXTRA_FILES)
        set(DEFAULT_ASSETS_EXTRA_FILES ${_nomi_stage})
    endif()
endif()
