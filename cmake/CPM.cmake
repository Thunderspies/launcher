# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2019-2023 Lars Melchior and contributors
# Adapted from CPM.cmake's get_cpm.cmake bootstrap.

set(CPM_DOWNLOAD_VERSION 0.43.1)
set(CPM_HASH_SUM
	1c40fc102ce9625d7de7eb14f541cab30cc3138dca627f0b0ec40293ce6c2934
)
set(cpm_filename "CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(CPM_PATH)
	set(CPM_DOWNLOAD_LOCATION "${CPM_PATH}/CPM.cmake")
elseif(DEFINED ENV{CPM_PATH})
	file(TO_CMAKE_PATH "$ENV{CPM_PATH}/CPM.cmake" CPM_DOWNLOAD_LOCATION)
elseif(CPM_SOURCE_CACHE)
	set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/${cpm_filename}")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
	set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/${cpm_filename}")
else()
	set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/${cpm_filename}")
endif()

get_filename_component(CPM_DOWNLOAD_LOCATION
	"${CPM_DOWNLOAD_LOCATION}" ABSOLUTE
)
set(cpm_release_url "https://github.com/cpm-cmake/CPM.cmake/releases/download")
file(DOWNLOAD
	"${cpm_release_url}/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
	"${CPM_DOWNLOAD_LOCATION}"
	EXPECTED_HASH SHA256=${CPM_HASH_SUM}
	TLS_VERIFY ON
)
include("${CPM_DOWNLOAD_LOCATION}")
