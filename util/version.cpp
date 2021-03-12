#include "gen_version.h"

#define LIB_VERSION_STR VERSION_STR " " GIT_COMMIT_ID "@" GIT_TIMESTAMP_ID

const char * LIB_VERSION = LIB_VERSION_STR;