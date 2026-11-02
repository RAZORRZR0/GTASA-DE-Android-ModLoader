#pragma once

#define CLEO_ANDROID		"CLEO ANDROID"
#define COPYRIGHT			"(C) Alexander Blade"
#define WEBPAGE				"http://dev-c.com"
#define WWW_WEBPAGE			"www.dev-c.com"
#define VERSION_STR 		CLEO_ANDROID " (" VERSION_DATE ")"
#define VERSION_DATE	 	"04 Apr 2021"
#define VERSION_CODE 		201
#define VERSION_CODE_STR 	"2.0.1"

#include "common.h"

namespace core
{
	enum e_game
	{
		GTA3,
		GTAVC,
		GTASA,
		GTALCS,
		GTAVCS
	};

	e_game GetGame();
	int GetGameVersion();

	const char *GetPackageName();
	const char *GetPackageVersionStr();
	int GetPackageVersionCode();

	uint16_t *GetGxtEntry(LPCSTR name, bool useCustom);

	// DE (arm64) core: libUE4.so symbol lookup and AML hooks; 32-bit script ip helpers and plugins are gone
	void *getsym(const char *name);
	void hook(void *fn, void *hook_fn, void **orig);
}