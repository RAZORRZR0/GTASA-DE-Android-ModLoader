// CLEO Android 2.0.1 for GTA SA: The Definitive Edition on Android (libUE4.so v1.87.0, arm64), as an AML mod.
// Port of cleolibrary/CLEO-ANDROID (BSD-3, see LICENSE): text.cpp, touch.cpp, strutils.cpp, utils.cpp, mutex.cpp,
// core_menu.h and the menu code of ui.cpp are its files; this file replaces core.cpp/main.cpp for DE.
//
// DE differences handled here:
//  - The script VM keeps PC SA semantics: negative jump offsets are relative to the script's base IP (+0x18), so a
//    script runs natively once base IP = its code; CLEO's jump/call rewriting is not needed. CRunningScript (0x130):
//    +0x18 base IP, +0x20 IP, +0x11C wake time. ProcessOneCommand is inlined into CRunningScript::Process; CLEO opcodes
//    (>= 0xA8C) arrive in ProcessCommands2600To2699, end_thread in ProcessCommands0To99.
//  - 64-bit addresses in 32-bit script variables: memory opcodes use the low 32 bits of an address in the 4 GB window
//    holding libUE4.so, and script code is placed in that window too. Heap pointers outside it are not reachable.
//  - Touch comes from LIB_PointerGetCoordinates (ui.cpp), menus draw in CHud::Draw at any render size.
//  - .so plugins (ARM32) and the Android menu key are not available.
#include <mod/amlmod.h>
#include <sys/mman.h>
#include "core.h"
#include "core_menu.h"
#include "strutils.h"
#include "text.h"
#include "touch.h"
#include "ui.h"
#include "utils.h"

MYMOD(net.gtamod.cleo, CLEO, 2.0.1, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

namespace core
{
	static void *ue4;
	static uintptr_t image_base, image_end, window_hi;
	e_game GetGame() { return GTASA; }
	int GetGameVersion() { return 0; } // VER_NONE: no CLEO Android binary layout applies to DE
	const char *GetPackageName() { return "com.rockstargames.gtasa.de"; }
	const char *GetPackageVersionStr() { return "1.87.0"; }
	int GetPackageVersionCode() { return 1087; }
	void *getsym(const char *name) { return (void *)aml->GetSym(ue4, name); }
	void hook(void *fn, void *hook_fn, void **orig) { aml->Hook(fn, hook_fn, orig); }

	static uint32_t V(const void *p) { return (uint32_t)(uintptr_t)p; }
	static uint8_t *R(uint32_t v) { return (uint8_t *)(window_hi | v); }
	static bool in_window(uintptr_t p) { return (p & ~(uintptr_t)0xFFFFFFFF) == window_hi; }

	// game
	static uint8_t *(*StartNewScript)(uint8_t *ip);
	static void (*CollectParameters_)(uint8_t *s, short count);
	static int32_t *(*GetPointerToScriptVariable_)(uint8_t *s, uint8_t type);
	static void (*RemoveScriptFromList)(uint8_t *s, uint8_t **list);
	static void (*AddScriptToList)(uint8_t *s, uint8_t **list);
	static int32_t *ScriptParams;
	static uint8_t **pActiveScripts;
	static uint32_t *TimeMs;
	static void CollectParameters(uint8_t *s, int n) { CollectParameters_(s, (short)n); }
	static int32_t *Var(uint8_t *s) { return GetPointerToScriptVariable_(s, 0); }

	struct t_script
	{
		std::string name;
		int32_t invokable_id = -1; // every script is a CLEO menu entry
		bool toggle = false;       // .csa: the menu turns it on/off; "on" scripts also start with each game
		bool enabled = false;
		uint8_t *handle = nullptr; // CRunningScript while running
		uint8_t *code = nullptr;
		uint32_t code_size = 0;
		uint64_t context[32] = {};
	};
	static std::vector<t_script *> scripts;
	static t_script *get_script_using_handle(uint8_t *handle)
	{
		for (t_script *s : scripts)
			if (s->handle == handle) return s;
		return nullptr;
	}
	struct t_mutex_var { uint32_t id, value; };
	static std::vector<t_mutex_var> mutex_vars;

	// script code arena inside the libUE4.so 4 GB window, so label addresses fit 32-bit variables
	static uint8_t *arena;
	static size_t arena_size = 16 << 20, arena_used;
	static bool alloc_arena()
	{
		for (uintptr_t hint = (image_end + 0x1000000) & ~(uintptr_t)0xFFFF; in_window(hint + arena_size - 1); hint += 0x4000000)
		{
			void *p = mmap((void *)hint, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED) continue;
			if (in_window((uintptr_t)p) && in_window((uintptr_t)p + arena_size - 1)) { arena = (uint8_t *)p; return true; }
			munmap(p, arena_size);
		}
		return false;
	}

	std::string get_storage_dir()
	{
		std::string root = aml->GetAndroidDataRootPath();
		if (!root.empty() && root.back() != '/') root += '/';
		return root + "cleo/";
	}

	// .csa on/off state, one enabled file name per line in cleo/enabled.txt
	static std::vector<std::string> enabled_csa;
	static void save_enabled()
	{
		std::string s;
		for (const std::string &n : enabled_csa) s += n + "\n";
		if (FILE *f = fopen((get_storage_dir() + "enabled.txt").c_str(), "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
	}
	static void load_enabled()
	{
		enabled_csa.clear();
		uint32_t size = 0;
		uint8_t *buf = utils::load_binary_file(get_storage_dir() + "enabled.txt", size);
		if (!buf) return;
		std::string s((const char *)buf, size), line;
		free(buf);
		for (char c : s + "\n")
			if (c == '\n' || c == '\r') { if (!line.empty()) enabled_csa.push_back(line); line.clear(); }
			else line += c;
	}

	static void set_menu_name(t_script *t)
	{
		std::string n = t->name.substr(0, t->name.size() - 4); // without .csa/.csi
		text::set_gxt_invokable_script_name(t->invokable_id, t->toggle ? n + (t->enabled ? " - on" : " - off") : n);
	}

	static void preload_script(std::string fname, const uint8_t *code, uint32_t code_size, bool is_invokable, uint32_t invokable_id)
	{
		if (arena_used + code_size > arena_size)
		{
			utils::log("no room for script '%s'", fname.c_str());
			return;
		}
		t_script *script = new t_script();
		script->code = arena + arena_used;
		arena_used = (arena_used + code_size + 15) & ~(size_t)15;
		memcpy(script->code, code, code_size);
		script->invokable_id = is_invokable ? invokable_id : -1;
		script->name = fname;
		script->code_size = code_size;
		script->toggle = strutils::get_ext(fname) == "csa";
		script->enabled = std::find(enabled_csa.begin(), enabled_csa.end(), fname) != enabled_csa.end();
		scripts.push_back(script);
		if (is_invokable)
			set_menu_name(script);
	}

	// CLEO Android preload_scripts(): menu script, *.csa, *.csi, *.fxt from the storage dir, re-read on every start
	static void preload_scripts()
	{
		std::string dir = get_storage_dir();
		std::vector<std::string> files;
		utils::list_files_in_dir(dir, files);
		arena_used = 0;
		load_enabled();
		for (t_script *s : scripts) delete s;
		scripts.clear();
		text::init();
		ui::delete_menu();
		preload_script("menu", sa_menu_script, sizeof(sa_menu_script), false, 0);
		uint32_t invokable_count = 0;
		for (const std::string &fname : files)
		{
			std::string ext = strutils::get_ext(fname);
			bool is_invokable = ext == "csi" || ext == "csa";
			if (is_invokable)
			{
				uint32_t code_size = 0;
				uint8_t *code = utils::load_binary_file(dir + fname, code_size);
				if (!code || !code_size)
				{
					utils::log("can't read script '%s'", fname.c_str());
					continue;
				}
				preload_script(fname, code, code_size, is_invokable, invokable_count);
				if (is_invokable) invokable_count++;
				free(code);
				utils::log("script '%s' with size %d preloaded", fname.c_str(), code_size);
			}
			else if (ext == "fxt")
			{
				uint32_t size = 0;
				LPSTR text_raw = cast<LPSTR>(utils::load_binary_file(dir + fname, size));
				if (!text_raw || !size)
				{
					utils::log("can't read fxt '%s'", fname.c_str());
					continue;
				}
				LPSTR txt = cast<LPSTR>(malloc(size + 1));
				memcpy(txt, text_raw, size);
				txt[size] = 0;
				text::load_gxt_entries_from_text(txt, size);
				free(txt);
				free(text_raw);
				utils::log("fxt '%s' loaded", fname.c_str());
			}
		}
		utils::log("total preloaded scripts %d", (int)scripts.size());
	}

	static void start_script(t_script *script, bool wait)
	{
		script->handle = StartNewScript(script->code);
		*(uint8_t **)(script->handle + 0x18) = script->code; // base IP: script-relative jumps
		if (wait) // custom scripts idle for a few seconds after the start, as in CLEO Android
			*(uint32_t *)(script->handle + 0x11C) = *TimeMs + 3000;
	}

	static void launch_scripts()
	{
		mutex_vars.clear();
		for (t_script *script : scripts)
		{
			script->handle = nullptr;
			if (script->invokable_id == -1 || (script->toggle && script->enabled))
			{
				utils::log("starting script '%s'", script->name.c_str());
				start_script(script, true);
			}
		}
	}

	// hooks
	static bool bCTheScripts__Load;
	static void (*CTheScripts__Init_)();
	static void CTheScripts__Init()
	{
		CTheScripts__Init_();
		if (!bCTheScripts__Load)
		{
			preload_scripts();
			launch_scripts();
		}
	}
	static void (*CTheScripts__Load_)();
	static void CTheScripts__Load()
	{
		bCTheScripts__Load = true;
		CTheScripts__Load_(); // calls Init
		bCTheScripts__Load = false;
		preload_scripts();
		launch_scripts();
	}
	// DE saves and loads scripts in CTheScripts::Serialize (FArchive flags at +0x28: bit 0 loading, bit 1 saving).
	// Saving: custom scripts stay out of the save game (DE can't load them back: it crashed on such saves).
	// Loading: Serialize calls Init and rebuilds the script lists, so custom scripts start after it.
	static void (*CTheScripts__Serialize_)(uint8_t *ar);
	static void CTheScripts__Serialize(uint8_t *ar)
	{
		uint32_t flags = *(uint32_t *)(ar + 0x28);
		if (flags & 2)
		{
			for (t_script *s : scripts)
				if (s->handle) RemoveScriptFromList(s->handle, pActiveScripts);
			CTheScripts__Serialize_(ar);
			for (t_script *s : scripts)
				if (s->handle) AddScriptToList(s->handle, pActiveScripts);
		}
		else if (flags & 1)
		{
			bCTheScripts__Load = true;
			CTheScripts__Serialize_(ar);
			bCTheScripts__Load = false;
			preload_scripts();
			launch_scripts();
		}
		else
			CTheScripts__Serialize_(ar);
	}

	ptr CTextHandle;
	static uint16_t *(*CText__Get_)(ptr thiz, LPCSTR name);
	static uint16_t *CText__Get(ptr thiz, LPCSTR name)
	{
		CTextHandle = thiz;
		uint16_t *e = text::get_gxt_entry(name);
		return e ? e : CText__Get_(thiz, name);
	}
	uint16_t *GetGxtEntry(LPCSTR name, bool useCustom)
	{
		if (!CTextHandle || !name || !name[0]) return NULL;
		uint16_t *e = useCustom ? text::get_gxt_entry(name) : NULL;
		return e ? e : CText__Get_(CTextHandle, name);
	}

	static bool (*ProcessCommands0To99_)(uint8_t *s, int op);
	static bool ProcessCommands0To99(uint8_t *s, int op)
	{
		if (op == 0x4E) // end_thread: the game moves the script to the idle list and reuses it
			if (t_script *script = get_script_using_handle(s))
			{
				utils::log("terminating script '%s'", script->name.c_str());
				script->handle = nullptr;
			}
		return ProcessCommands0To99_(s, op);
	}

	static const uint8_t end_thread[] = { 0x4E, 0x00 };
	static void end_script(uint8_t *s) { *(const uint8_t **)(s + 0x20) = end_thread; }

	// 8-byte string param read raw at the IP, as CLEO Android's SA read_str_8byte
	static bool read_str_8byte(uint8_t *s, std::string &str)
	{
		uint8_t *&ip = *(uint8_t **)(s + 0x20);
		str.assign((const char *)ip, strnlen((const char *)ip, 8));
		ip += 8;
		return true;
	}

	static bool find_pattern(uint32_t &result, const char *pattern, int index)
	{
		std::vector<int> bytes; // -1 = wildcard
		for (const char *p = pattern; *p;)
		{
			if (*p == ' ') { p++; continue; }
			if (*p == '?') { bytes.push_back(-1); p += p[1] == '?' ? 2 : 1; continue; }
			char hex[3] = { p[0], p[1], 0 };
			bytes.push_back((int)strtol(hex, nullptr, 16));
			p += 2;
		}
		if (bytes.empty()) return false;
		for (uint8_t *a = (uint8_t *)image_base; a + bytes.size() <= (uint8_t *)image_end; a++)
		{
			size_t i = 0;
			while (i < bytes.size() && (bytes[i] < 0 || a[i] == bytes[i])) i++;
			if (i == bytes.size() && index-- == 0) { result = V(a); return true; }
		}
		return false;
	}

	static void mem_write(uint8_t *addr, const void *val, uint32_t size, bool protect)
	{
		if (protect)
		{
			uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xFFF;
			mprotect((void *)page, ((uintptr_t)addr + size - page + 0xFFF) & ~(uintptr_t)0xFFF, PROT_READ | PROT_WRITE | PROT_EXEC);
		}
		memcpy(addr, val, size);
		if (protect) __builtin___clear_cache((char *)addr, (char *)addr + size); // code patches
	}

	// CLEO Android custom_opcode(), with DE address handling
	static bool custom_opcode(t_script &script, uint8_t *s, uint16_t op)
	{
		switch (op)
		{
		case 0x0DD0: // get_label_addr
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			int32_t off = ScriptParams[0];
			*v = V(off >= 0 ? script.code + off : script.code - off);
			return true;
		}
		case 0x0DD1: // get_func_addr_by_cstr_name
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			*v = V(getsym((const char *)R(ScriptParams[0])));
			return true;
		}
		case 0x0DD2: // context_call_func
		{
			CollectParameters(s, 1);
			typedef uint64_t (*fn6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
			uint64_t *c = script.context;
			c[0] = ((fn6)R(ScriptParams[0]))(c[0], c[1], c[2], c[3], c[4], c[5]);
			return true;
		}
		case 0x0DD3: // context_set_reg
			CollectParameters(s, 2);
			script.context[ScriptParams[0] & 31] = (uint32_t)ScriptParams[1];
			return true;
		case 0x0DD4: // context_get_reg
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			*v = (int32_t)script.context[ScriptParams[0] & 31];
			return true;
		}
		case 0x0DD5: *Var(s) = 1; return true; // get_platform: Android
		case 0x0DD6: *Var(s) = GetGameVersion(); return true;
		case 0x0DD7: *Var(s) = V((void *)image_base); return true;
		case 0x0DD8: // read_mem_addr
		{
			int32_t *v = Var(s);
			CollectParameters(s, 3);
			uint32_t addr = ScriptParams[0], size = ScriptParams[1];
			if (ScriptParams[2]) addr += V((void *)image_base);
			*v = 0;
			memcpy(v, R(addr), size > 4 ? 4 : size);
			return true;
		}
		case 0x0DD9: // write_mem_addr
		{
			CollectParameters(s, 5);
			uint32_t addr = ScriptParams[0], size = ScriptParams[2];
			int32_t val = ScriptParams[1];
			if (ScriptParams[3]) addr += V((void *)image_base);
			mem_write(R(addr), &val, size > 4 ? 4 : size, ScriptParams[4] != 0);
			return true;
		}
		case 0x0DDA: // search_mem
		{
			int32_t *v = Var(s);
			CollectParameters(s, 2);
			uint32_t r = 0;
			*v = find_pattern(r, (const char *)R(ScriptParams[0]), ScriptParams[1]) ? r : 0;
			return true;
		}
		case 0x0DDB: // get_game_version_ex
		{
			int32_t *id = Var(s), *ver = Var(s), *ver_code = Var(s);
			*id = strutils::str_hash(GetPackageName());
			*ver = strutils::str_hash(GetPackageVersionStr());
			*ver_code = GetPackageVersionCode();
			return true;
		}
		case 0x0DDC: // set_mutex_var
		{
			CollectParameters(s, 2);
			for (t_mutex_var &m : mutex_vars)
				if (m.id == (uint32_t)ScriptParams[0]) { m.value = ScriptParams[1]; return true; }
			mutex_vars.push_back({ (uint32_t)ScriptParams[0], (uint32_t)ScriptParams[1] });
			return true;
		}
		case 0x0DDD: // get_mutex_var
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			*v = 0;
			for (t_mutex_var &m : mutex_vars)
				if (m.id == (uint32_t)ScriptParams[0]) { *v = m.value; break; }
			return true;
		}
		case 0x0DDE: // call_func: AAPCS64 puts ints in x0..x7 and floats in s0..s7
		{
			CollectParameters(s, 2);
			uint32_t addr = ScriptParams[0];
			if (ScriptParams[1]) addr += V((void *)image_base);
			uint64_t ia[8] = {};
			float fa[8] = {};
			int ni = 0, nf = 0;
			int32_t *res_ptr = nullptr;
			for (;;)
			{
				uint8_t *&ip = *(uint8_t **)(s + 0x20);
				if (*ip == 0) { ip++; break; }
				std::string str;
				read_str_8byte(s, str);
				str = strutils::str_to_lower(str);
				if (str == "i")
				{
					CollectParameters(s, 1);
					uint32_t u = ScriptParams[0];
					// 32-bit address of the libUE4 window or script code: pass the full pointer
					bool addr_like = (u >= V((void *)image_base) && u < V((void *)image_end)) || (u >= V(arena) && u < V(arena + arena_size));
					if (ni < 8) ia[ni++] = addr_like ? (uint64_t)R(u) : (uint64_t)(int64_t)ScriptParams[0];
				}
				else if (str == "f")
				{
					CollectParameters(s, 1);
					if (nf < 8) memcpy(&fa[nf++], &ScriptParams[0], 4);
				}
				else if (str == "ref")
				{
					int32_t *p = Var(s);
					if (ni < 8) ia[ni++] = (uint64_t)p;
				}
				else if (str == "resi" || str == "resf")
					res_ptr = Var(s);
				else
				{
					utils::log("'%s': func call has unknown param type '%s', ending the script", script.name.c_str(), str.c_str());
					end_script(s);
					return true;
				}
			}
			typedef uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
								   float, float, float, float, float, float, float, float);
			uint64_t res = ((fn)R(addr))(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5], ia[6], ia[7],
										fa[0], fa[1], fa[2], fa[3], fa[4], fa[5], fa[6], fa[7]);
			if (res_ptr) *res_ptr = (int32_t)res;
			return true;
		}
		case 0x0DE0: // get_touch_point_state
		{
			int32_t *v = Var(s);
			CollectParameters(s, 2);
			*v = touch::point_touched_timed(ScriptParams[0], ScriptParams[1]) ? 1 : 0;
			return true;
		}
		case 0x0DE1: // get_touch_slide_state
		{
			int32_t *v = Var(s);
			CollectParameters(s, 4);
			*v = touch::point_slide_done(ScriptParams[0], ScriptParams[1], ScriptParams[2], ScriptParams[3]) ? 1 : 0;
			return true;
		}
		case 0x0DE2: *Var(s) = touch::menu_button_pressed() ? 1 : 0; return true;
		case 0x0DE3:
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			*v = touch::menu_button_pressed_timed(ScriptParams[0]) ? 1 : 0;
			return true;
		}
		case 0x0DE4: { int32_t *v = Var(s); CollectParameters(s, 1); *v = 0; return true; } // PSP controls
		case 0x0DE5: { int32_t *v = Var(s); CollectParameters(s, 2); *v = 0; return true; }
		case 0x0DEE: // invokable_script_stats
		{
			uint32_t inv_count = 0;
			for (t_script *t : scripts)
				if (t->invokable_id != -1) inv_count++;
			int32_t *p_count = Var(s), *p_page_count = Var(s);
			*p_count = inv_count;
			*p_page_count = inv_count / 12 + 1;
			return true;
		}
		case 0x0DEF: // start_invokable_script
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			for (t_script *t : scripts)
				if (t->invokable_id == ScriptParams[0])
				{
					if (t->toggle) // .csa: flip on/off, start or stop it now
					{
						t->enabled = !t->enabled;
						enabled_csa.erase(std::remove(enabled_csa.begin(), enabled_csa.end(), t->name), enabled_csa.end());
						if (t->enabled) enabled_csa.push_back(t->name);
						save_enabled();
						set_menu_name(t);
						utils::log("script '%s' %s", t->name.c_str(), t->enabled ? "on" : "off");
						if (t->enabled && !t->handle)
							start_script(t, false);
						else if (!t->enabled && t->handle)
						{
							end_script(t->handle);
							*(uint32_t *)(t->handle + 0x11C) = 0; // wake it so it ends now
						}
						*v = 0;
						return true;
					}
					if (t->handle)
					{
						utils::log("invokable script '%s' already running", t->name.c_str());
						*v = 2;
						return true;
					}
					utils::log("starting invokable script '%s'", t->name.c_str());
					start_script(t, false);
					*v = 0;
					return true;
				}
			utils::log("invokable script %d not found", ScriptParams[0]);
			*v = -1;
			return true;
		}
		case 0x0DF0: ui::show_arrow(); return true;
		case 0x0DF1: ui::hide_arrow(); return true;
		case 0x0DF2: // create_menu
		{
			CollectParameters(s, 2);
			const char *a = (const char *)R(ScriptParams[0]);
			uint32_t item_count = ScriptParams[1];
			uint8_t flags = *a;
			bool bItemsUseGxt = flags & 1;
			uint32_t maxItemStrSize = (flags & 2) ? 20 : 0;
			a += 4;
			std::string title = a;
			a += title.size() + 1;
			std::string titleclose = a;
			a += titleclose.size() + 1;
			std::vector<wide_string> items;
			while (*a && item_count)
			{
				std::string a_item = a;
				a += a_item.size() + 1;
				wide_string w_item(a_item.begin(), a_item.end());
				bool bGxtEntryExists = false;
				if (bItemsUseGxt)
					if (uint16_t *wstr = GetGxtEntry(a_item.c_str(), true))
						if (strutils::ansi_from_wstr(wstr).size())
						{
							bGxtEntryExists = true;
							w_item = wstr;
						}
				if (bGxtEntryExists && maxItemStrSize && w_item.size() > maxItemStrSize)
					w_item.erase(maxItemStrSize);
				items.push_back(w_item);
				item_count--;
			}
			ui::create_menu(title, titleclose, items);
			return true;
		}
		case 0x0DF3: ui::delete_menu(); return true;
		case 0x0DF4:
		{
			int32_t *v = Var(s);
			CollectParameters(s, 1);
			*v = ui::get_menu_touched_item_index(ScriptParams[0]);
			return true;
		}
		case 0x0DF5: CollectParameters(s, 1); ui::set_menu_active_item_index(ScriptParams[0]); return true;
		case 0x0DF6: *Var(s) = ui::get_menu_active_item_index(); return true;
		}
		return false;
	}

	// game panel menu for CLEO scripts (DE doesn't show it and crashes on its later use): ui.cpp draws it
	static void (*ReadTextLabelFromScript)(uint8_t *s, char *buf, uint8_t len);
	static wide_string read_gxt(uint8_t *s, bool *dummy = nullptr)
	{
		char key[9] = {};
		ReadTextLabelFromScript(s, key, 8);
		if (dummy) *dummy = !strcmp(key, "DUMMY"); // unused row
		if (uint16_t *w = GetGxtEntry(key, true)) return w;
		return wide_string(key, key + strlen(key));
	}
	static bool (*ProcessCommands2200To2299_)(uint8_t *s, int op);
	static bool ProcessCommands2200To2299(uint8_t *s, int op)
	{
		if ((op != 0x08D4 && op != 0x08DA && op != 0x08DB) || !get_script_using_handle(s))
			return ProcessCommands2200To2299_(s, op);
		if (op == 0x08D4) // CREATE_MENU title x y width columns interactive background align -> menu
		{
			read_gxt(s);
			CollectParameters(s, 7);
			*Var(s) = 1;
			ui::panel_create();
		}
		else if (op == 0x08DA) // DELETE_MENU
		{
			CollectParameters(s, 1);
			ui::panel_delete();
		}
		else // SET_MENU_COLUMN menu column header row0..row11
		{
			CollectParameters(s, 2);
			wide_string header = read_gxt(s);
			std::vector<wide_string> rows;
			for (int i = 0; i < 12; i++)
			{
				bool dummy;
				wide_string r = read_gxt(s, &dummy);
				if (!dummy) rows.push_back(r);
			}
			ui::panel_set_column(header, rows);
		}
		return false;
	}
	static bool (*ProcessCommands2300To2399_)(uint8_t *s, int op);
	static bool ProcessCommands2300To2399(uint8_t *s, int op)
	{
		if (op != 0x090E || !get_script_using_handle(s))
			return ProcessCommands2300To2399_(s, op);
		CollectParameters(s, 2); // SET_ACTIVE_MENU_ITEM menu row
		ui::panel_set_active_row(ScriptParams[1]);
		return false;
	}

	// CLEO opcodes (and every opcode >= 0xA8C) reach the game here, IP already past the opcode
	static bool (*ProcessCommands2600To2699_)(uint8_t *s, int op);
	static bool ProcessCommands2600To2699(uint8_t *s, int op)
	{
		t_script *script = (op >= 0x0DD0 && op <= 0x0DFF) || op == 0x0A93 ? get_script_using_handle(s) : nullptr;
		if (!script) return ProcessCommands2600To2699_(s, op);
		if (op == 0x0A93) // terminate_this_custom_script
			end_script(s);
		else if (!custom_opcode(*script, s, op))
		{
			utils::log("'%s': unknown opcode %04X, ending the script", script->name.c_str(), op);
			end_script(s);
		}
		return false;
	}

	static bool initialize()
	{
		ue4 = (void *)aml->GetLibHandle("libUE4.so");
		if (!ue4) return false;
		Dl_info info;
		void *hud = getsym("_ZN4CHud4DrawEv");
		if (!hud || !dladdr(hud, &info)) return false;
		image_base = (uintptr_t)info.dli_fbase;
		image_end = (uintptr_t)getsym("_end"); // end of libUE4.so's bss
		if (!image_end) image_end = image_base + 0x10000000;
		window_hi = image_base & ~(uintptr_t)0xFFFFFFFF;
		if (!in_window(image_end - 1)) utils::log("libUE4.so crosses a 4 GB boundary: memory opcodes above it fail");
		StartNewScript = (decltype(StartNewScript))getsym("_ZN11CTheScripts14StartNewScriptEPh");
		CollectParameters_ = (decltype(CollectParameters_))getsym("_ZN14CRunningScript17CollectParametersEs");
		GetPointerToScriptVariable_ = (decltype(GetPointerToScriptVariable_))getsym("_ZN14CRunningScript26GetPointerToScriptVariableEh");
		RemoveScriptFromList = (decltype(RemoveScriptFromList))getsym("_ZN14CRunningScript20RemoveScriptFromListEPPS_");
		AddScriptToList = (decltype(AddScriptToList))getsym("_ZN14CRunningScript15AddScriptToListEPPS_");
		ScriptParams = (int32_t *)getsym("ScriptParams");
		pActiveScripts = (uint8_t **)getsym("_ZN11CTheScripts14pActiveScriptsE");
		TimeMs = (uint32_t *)getsym("_ZN6CTimer22m_snTimeInMillisecondsE");
		void *init = getsym("_ZN11CTheScripts4InitEv"), *load = getsym("_ZN11CTheScripts4LoadEv");
		void *serialize = getsym("_ZN11CTheScripts9SerializeER8FArchive");
		void *cmd0 = getsym("_ZN14CRunningScript20ProcessCommands0To99Ei");
		void *cmd2600 = getsym("_ZN14CRunningScript25ProcessCommands2600To2699Ei");
		void *cmd2200 = getsym("_ZN14CRunningScript25ProcessCommands2200To2299Ei");
		void *cmd2300 = getsym("_ZN14CRunningScript25ProcessCommands2300To2399Ei");
		ReadTextLabelFromScript = (decltype(ReadTextLabelFromScript))getsym("_ZN14CRunningScript23ReadTextLabelFromScriptEPch");
		void *textget = getsym("_ZN5CText3GetEPKc");
		if (!(StartNewScript && CollectParameters_ && GetPointerToScriptVariable_ && RemoveScriptFromList && AddScriptToList &&
			  ScriptParams && pActiveScripts && TimeMs && init && load && serialize && cmd0 && cmd2600 && cmd2200 && cmd2300 &&
			  ReadTextLabelFromScript && textget))
			return false;
		if (!alloc_arena()) return false;
		if (!ui::init()) return false;
		hook(init, (void *)CTheScripts__Init, (void **)&CTheScripts__Init_);
		hook(load, (void *)CTheScripts__Load, (void **)&CTheScripts__Load_);
		hook(serialize, (void *)CTheScripts__Serialize, (void **)&CTheScripts__Serialize_);
		hook(cmd0, (void *)ProcessCommands0To99, (void **)&ProcessCommands0To99_);
		hook(cmd2600, (void *)ProcessCommands2600To2699, (void **)&ProcessCommands2600To2699_);
		hook(cmd2200, (void *)ProcessCommands2200To2299, (void **)&ProcessCommands2200To2299_);
		hook(cmd2300, (void *)ProcessCommands2300To2399, (void **)&ProcessCommands2300To2399_);
		hook(textget, (void *)CText__Get, (void **)&CText__Get_);
		return true;
	}
}

ON_MOD_LOAD()
{
	if (core::initialize())
		utils::log(VERSION_STR " for GTA SA DE: scripts from %s", core::get_storage_dir().c_str());
	else
		utils::log("ABORT: unsupported game version (libUE4.so symbol missing) or no memory for scripts");
}
