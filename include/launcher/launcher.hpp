#ifndef LAUNCHER_LAUNCHER_HPP
#define LAUNCHER_LAUNCHER_HPP

#include <launcher/launcher.h>

/*
	* Procedural C++ interface: identical contracts and ownership to the C
	* API. Names are imported, rather than wrapped in classes or a second
	* ABI.
	*/
namespace launcher
{
using ::launcher_apply;
using ::launcher_close;
using ::launcher_config;
using ::launcher_configure;
using ::launcher_decode_channel;
using ::launcher_decode_release;
using ::launcher_error;
using ::launcher_encoding;
using ::LAUNCHER_RAW;
using ::LAUNCHER_ZLIB;
using ::launcher_file;
using ::launcher_file_repository;
using ::launcher_descriptor_fn;
using ::launcher_file_kind;
using ::launcher_get_config;
using ::launcher_get_installed;
using ::launcher_handle;
using ::launcher_launch;
using ::launcher_launch_info;
using ::launcher_make_plan;
using ::launcher_open;
using ::launcher_plan;
using ::launcher_plan_free;
using ::launcher_plan_get_info;
using ::launcher_plan_info;
using ::launcher_progress;
using ::launcher_progress_fn;
using ::launcher_release;
using ::launcher_file_group;
using ::launcher_release_group_count;
using ::launcher_release_group;
using ::launcher_release_group_file;
using ::launcher_release_free;
using ::launcher_release_get_launch;
using ::launcher_release_id;
using ::launcher_repair;
using ::launcher_s3_credentials;
using ::launcher_set_s3_credentials;
using ::launcher_set_file_s3_credentials;
using ::launcher_resolve;
using ::launcher_status;
using ::launcher_status_name;
using ::launcher_update;
using ::launcher_verify;
} // namespace launcher

#endif
