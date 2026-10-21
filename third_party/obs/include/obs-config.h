#pragma once

#ifndef MAKE_SEMANTIC_VERSION
#define MAKE_SEMANTIC_VERSION(major, minor, patch) \
	(((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) | (uint32_t)(patch))
#endif

#define LIBOBS_API_MAJOR_VER 32
#define LIBOBS_API_MINOR_VER 2
#define LIBOBS_API_PATCH_VER 2
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER)

#define OBS_VERSION "32.2.2"
#define OBS_DATA_PATH "../../data"
#define OBS_INSTALL_PREFIX ""
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
