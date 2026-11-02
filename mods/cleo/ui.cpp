#include "ui.h"
#include "utils.h"
#include "touch.h"
#include "strutils.h"

namespace ui
{
	typedef void (*fn_Draw_Poly)(float, float, float, float, float, float, float, float, uint8_t *);
	fn_Draw_Poly draw_poly;

	enum eAlign
	{
		eAlignCenter,
		eAlignLeft,
		eAlignRight
	};

	enum eStyle
	{
		eStyleSimple,
		eStyleClassic,
		eStyleSpecial
	};

	typedef void (*fn_Print_String)(uint16_t *str, float x, float y, eAlign align, float scalex, float scaley, uint8_t *rgba, eStyle style);
	fn_Print_String print_string;

	void on_draw();

	uint8_t menu_arrow_color[4];
	uint8_t menu_arrow_selected_alpha;
	uint8_t menu_active_item_font_color[4];
	uint8_t menu_selected_item_font_color[4];

	static uint16_t unistr[512];
	uint16_t *ansi_to_unicode(LPCSTR str, bool localize = false)
	{
	    strutils::wstr_from_ansi(unistr, str);
	    return unistr;
	}

	// draws arrow, length is 0..1
	void draw_arrow(float length)
	{
		uint8_t arrow_color[4] = { 255, 10, 10, 190 };
		float width = 0.08, top = 0.15, left = 0.5 - width / 2, h = length / 1.75;
		draw_poly(left, top, left + width, top, left, top + h, left + width, top + h, arrow_color);
		float aextent = 0.027, awidth = 0.12;
		draw_poly(left - aextent, top + h, left + width + aextent, top + h, 0.5, top + h + awidth, 0.5, top + h + awidth, arrow_color);
		uint8_t font_color[4] = { 252, 250, 250, 255 };
		print_string(ansi_to_unicode("OPEN CLEO MENU", true), 0.5, top - 0.05, eAlignCenter, 0.65, 1.5, font_color, eStyleClassic);
	}

	uint32_t arrow_draw_until_time;		// time in ticks til arrow must be drawn
	uint32_t arrow_frames;				// indicates arrow current draw stage

	// shows arrow, called from scripts
	void show_arrow()
	{
		static bool bShowArrow = true;	// arrow shows only at start
		if (bShowArrow)
		{
			bShowArrow = false;
			const uint32_t arrow_draw_time = 7000;
			arrow_draw_until_time = utils::get_tick_count() + arrow_draw_time;
			arrow_frames = 0;
		}
	}

	// hides arrow, called from scripts
	void hide_arrow()
	{
		arrow_draw_until_time = 0;
		arrow_frames = 0;
	}

	// menu

	std::string menu_name;								// title
	std::string menu_close_name;						// close button title
	std::vector<wide_string> menu_items;				// item names
	int32_t menu_active_page = -1;						// active page index, -1 when menu is not active
	int32_t menu_active_item = -1;						// active item index in menu_items
	int32_t menu_page_count;							// total page count, set on create
	int32_t menu_selected_item;							// selected item index in menu_items, updated on touch handle
	uint32_t menu_selected_item_time;					// time in ticks when menu_selected_item was selected
	const int32_t menu_items_per_page = 8;				// max items per page
	int32_t menu_arrow_left_highlight_frames_left;		// frames to draw left arrow highlighted
	int32_t menu_arrow_right_highlight_frames_left;		// frames to draw right arrow highlighted
	int32_t menu_item_highlight_frames_left;			// frames to draw selected item highlighted
	const int32_t menu_highlight_frames = 3;			// frame count for highlight to be active
	// updated on draw
	float menu_close_points[4];							// close button rect coords					[x1,y1,x2,y2]
	float menu_arrow_left_points[8];					// left arrow coords for checking touches 	[x1,y1,x2,y2] * 4
	float menu_arrow_right_points[8];					// right arrow coords for checking touches 	[x1,y1,x2,y2] * 4
	float menu_item_points[menu_items_per_page * 4];	// item coords for checking touches 		[x1,y1,x2,y2] * menu_items_per_page
	uint32_t menu_item_points_current;					// current page item count
	uint32_t hud_draw_time;

	// creates menu, called from scripts
	void create_menu(std::string name, std::string closename, std::vector<wide_string> &items)
	{
		menu_name = name;
		menu_close_name = closename;
		menu_items = items;
		menu_active_page = 0;
		menu_active_item = 0;
		menu_selected_item = -1;
		menu_selected_item_time = 0;
		menu_arrow_left_highlight_frames_left = 0;
		menu_arrow_right_highlight_frames_left = 0;
		menu_item_highlight_frames_left = 0;
		menu_page_count = menu_items.size() / menu_items_per_page;
		if (menu_items.size() % menu_items_per_page)
			menu_page_count++;
	}

	// get menu touched item index
	// resets touch state
	// returns -1 if no menu touch occured, -2 for close button, 0 and above for item indexes
	// maxtime - touch valid time
	int32_t get_menu_touched_item_index(uint32_t maxtime)
	{
		if (utils::get_tick_count() < menu_selected_item_time + maxtime)
		{
			// utils::log("menu_selected_item %d", menu_selected_item);
			menu_selected_item_time = 0; // reset touch state
			return menu_selected_item;
		}
		return -1;
	}

	// deletes menu, called from scripts or on restart
	void delete_menu()
	{
		menu_active_page = -1;
		menu_active_item = -1;
		menu_selected_item = -1;
		menu_selected_item_time = 0;
		menu_page_count = 0;
	}

	void set_menu_active_item_index(int32_t index)
	{
		if (index >= 0 && index < menu_items.size())
		{
			menu_active_item = index;
			menu_active_page = menu_active_item / menu_items_per_page;
		}
	}

	int32_t get_menu_active_item_index()
	{
		return menu_active_item;
	}
	
	// checks if coords are inside properly defined rect
	bool coords_inside_rect(float x, float y, float x1, float y1, float x2, float y2)
	{
		return (x >= x1 && y >= y1 && x <= x2 && y <= y2);
	}

	// checks if coords are approximately inside 2d poly with 4 verts
	bool coords_approx_inside_rect(float x, float y, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4)
	{
		float center_x = (x1 + x2 + x3 + x4) / 4, center_y = (y1 + y2 + y3 + y4) / 4;
		float radius = sqrt((x1 - center_x) * (x1 - center_x) + (y1 - center_y) * (y1 - center_y));
		radius += radius / 2;

		float dist = sqrt((x - center_x) * (x - center_x) + (y - center_y) * (y - center_y));
		return dist < radius;
	}

	// handles touches, called in @touch::touch_event()
	void handle_touch(float x, float y)
	{
		// check if menu is active
		if (menu_active_page == -1) return;
		// items
		for (int32_t i = 0; i < menu_item_points_current; i++)
		{
			if (coords_inside_rect(x, y, menu_item_points[i * 4], menu_item_points[i * 4 + 1], menu_item_points[i * 4 + 2], menu_item_points[i * 4 + 3]))
			{
				menu_selected_item = menu_active_page * menu_items_per_page + i;
				menu_selected_item_time = utils::get_tick_count();
				menu_item_highlight_frames_left = menu_highlight_frames;
				return;
			}
		}
		// close button
		if (coords_inside_rect(x, y, menu_close_points[0], menu_close_points[1], menu_close_points[2], menu_close_points[3]))
		{
			menu_selected_item = -2;
			menu_selected_item_time = utils::get_tick_count();
			return;
		}
		// arrows
		if (menu_page_count > 1)
		{
			if (coords_approx_inside_rect(x, y, menu_arrow_left_points[0], menu_arrow_left_points[1], menu_arrow_left_points[2], menu_arrow_left_points[3],
												menu_arrow_left_points[4], menu_arrow_left_points[5], menu_arrow_left_points[6], menu_arrow_left_points[7]))
			{
				if (menu_active_page-- == 0)
					menu_active_page = menu_page_count - 1;
				menu_active_item = menu_active_page * menu_items_per_page; // compat
				menu_arrow_left_highlight_frames_left = menu_highlight_frames;
				return;
			}
			if (coords_approx_inside_rect(x, y, menu_arrow_right_points[0], menu_arrow_right_points[1], menu_arrow_right_points[2], menu_arrow_right_points[3],
												menu_arrow_right_points[4], menu_arrow_right_points[5], menu_arrow_right_points[6], menu_arrow_right_points[7]))
			{
				if (menu_active_page++ == menu_page_count - 1)
					menu_active_page = 0;
				menu_active_item = menu_active_page * menu_items_per_page; // compat
				menu_arrow_right_highlight_frames_left = menu_highlight_frames;
				return;
			}
		}
	}

	// menu must be active in order this to be called
	void draw_menu()
	{
		// draw rect
		float width = 0.3, height = 0.8, top = 0.12, left = 0.5 - width / 2;
		uint8_t rect_color[4] = { 55, 127, 175, 150 };
		draw_poly(left, top, left + width, top, left, top + height, left + width, top + height, rect_color);
		// draw title
		uint8_t font_color[4] = { 252, 250, 250, 255 };
		print_string(ansi_to_unicode(menu_name.c_str()), 0.5, top - 0.03, eAlignCenter, 0.8, 1.8, font_color, eStyleClassic);
		// draw close button
		menu_close_points[0] = 0.2;
		menu_close_points[1] = 0.87;
		menu_close_points[2] = menu_close_points[0] + 0.1;
		menu_close_points[3] = menu_close_points[1] + 0.1;
		//draw_poly(menu_close_points[0], menu_close_points[1], menu_close_points[2], menu_close_points[1],
		//		  menu_close_points[0], menu_close_points[3], menu_close_points[2], menu_close_points[3], rect_color);
		print_string(ansi_to_unicode(menu_close_name.c_str()), menu_close_points[0], menu_close_points[1], eAlignLeft, 0.6, 1.6, font_color, eStyleClassic);
		// draw page num if needed
		if (menu_page_count > 1)
		{
			char str[32];
			sprintf(str, "%d of %d", menu_active_page + 1, menu_page_count);
			print_string(ansi_to_unicode(str), 0.61, top + 0.04, eAlignCenter, 0.4f, 0.8f, font_color, eStyleSimple);
		}
		// draw items
		if (menu_active_page < 0 || menu_active_page > menu_page_count - 1)
			menu_active_page = 0;
		int32_t start_item = menu_active_page * menu_items_per_page;
		int32_t menu_items_this_page = (start_item + menu_items_per_page > menu_items.size()) ? menu_items.size() - start_item : menu_items_per_page;
		top = 0.2;
		menu_item_points_current = menu_items_this_page;
		for (int32_t i = 0; i < menu_items_this_page; i++)
		{
			uint8_t font_color[4] = { 252, 250, 250, 255 };
			if (menu_selected_item == start_item + i && menu_item_highlight_frames_left)
			{
				memcpy(font_color, menu_selected_item_font_color, sizeof(font_color));
				menu_item_highlight_frames_left--;
			}

			print_string((uint16_t *)menu_items[start_item + i].c_str(), 0.5, top, eAlignCenter, 0.5, 1.5, font_color, eStyleSimple);
			top += 0.01;
			menu_item_points[i * 4] = 0.5 - width / 2.5;
			menu_item_points[i * 4 + 1] = top;
			top += 0.07;
			menu_item_points[i * 4 + 2] = 0.5 + width / 2.5;
			menu_item_points[i * 4 + 3] = top;
			top += 0.01;
		}
		// draw arrows if needed
		if (menu_page_count > 1)
		{
			// left arrow
			float awidth = 0.08, aheight = 0.2, aextent = 0.015, right = left + width;
			uint8_t arrow_left_color[4];
			memcpy(arrow_left_color, menu_arrow_color, sizeof(menu_arrow_color));
			if (menu_arrow_left_highlight_frames_left)
			{
				arrow_left_color[3] = menu_arrow_selected_alpha;
				menu_arrow_left_highlight_frames_left--;
			}
			menu_arrow_left_points[0] = left - awidth - aextent;
			menu_arrow_left_points[1] = 0.5;
			menu_arrow_left_points[2] = left - aextent;
			menu_arrow_left_points[3] = 0.5 - aheight / 2;
			menu_arrow_left_points[4] = left - aextent;
			menu_arrow_left_points[5] = 0.5 + aheight / 2;
			menu_arrow_left_points[6] = left - awidth - aextent;
			menu_arrow_left_points[7] = 0.5;
			draw_poly(menu_arrow_left_points[0], menu_arrow_left_points[1], menu_arrow_left_points[2], menu_arrow_left_points[3],
					  menu_arrow_left_points[6], menu_arrow_left_points[7],
					  menu_arrow_left_points[4], menu_arrow_left_points[5],
					  arrow_left_color);
			// right arrow
			uint8_t arrow_right_color[4];
			memcpy(arrow_right_color, menu_arrow_color, sizeof(menu_arrow_color));
			if (menu_arrow_right_highlight_frames_left)
			{
				arrow_right_color[3] = menu_arrow_selected_alpha;
				menu_arrow_right_highlight_frames_left--;
			}
			menu_arrow_right_points[0] = right + awidth + aextent;
			menu_arrow_right_points[1] = 0.5;
			menu_arrow_right_points[2] = right + aextent;
			menu_arrow_right_points[3] = 0.5 - aheight / 2;
			menu_arrow_right_points[4] = right + aextent;
			menu_arrow_right_points[5] = 0.5 + aheight / 2;
			menu_arrow_right_points[6] = right + awidth + aextent;
			menu_arrow_right_points[7] = 0.5;
			draw_poly(menu_arrow_right_points[0], menu_arrow_right_points[1], menu_arrow_right_points[2], menu_arrow_right_points[3],
					  menu_arrow_right_points[6], menu_arrow_right_points[7],
					  menu_arrow_right_points[4], menu_arrow_right_points[5],
					  arrow_right_color);
		}
	}

	// game panel menu (08D4 CREATE_MENU family) for CLEO scripts: DE does not show it, so CLEO draws it
	// in the CLEO menu's style. One column, up to 12 rows of text, the active row highlighted.
	bool panel_active;
	wide_string panel_header;
	std::vector<wide_string> panel_rows;
	int32_t panel_active_row;
	void panel_create() { panel_active = true; panel_header.clear(); panel_rows.clear(); panel_active_row = 0; }
	void panel_set_column(const wide_string &header, const std::vector<wide_string> &rows) { panel_header = header; panel_rows = rows; }
	void panel_set_active_row(int32_t row) { panel_active_row = row; }
	void panel_delete() { panel_active = false; }
	void draw_panel()
	{
		float width = 0.3, height = 0.8, top = 0.12, left = 0.5 - width / 2;
		uint8_t rect_color[4] = { 55, 127, 175, 150 };
		draw_poly(left, top, left + width, top, left, top + height, left + width, top + height, rect_color);
		uint8_t font_color[4] = { 252, 250, 250, 255 };
		print_string((uint16_t *)panel_header.c_str(), 0.5, top - 0.03, eAlignCenter, 0.8, 1.8, font_color, eStyleClassic);
		top = 0.2;
		for (int32_t i = 0; i < (int32_t)panel_rows.size(); i++)
		{
			uint8_t *c = i == panel_active_row ? menu_selected_item_font_color : font_color;
			print_string((uint16_t *)panel_rows[i].c_str(), 0.5, top, eAlignCenter, 0.5, 1.5, c, eStyleSimple);
			top += 0.052; // 12 rows end above the script's DOWN label
		}
	}

	// common
	void on_draw()
	{
		// arrow
		uint32_t time = utils::get_tick_count();
		hud_draw_time = time;
		if (time < arrow_draw_until_time || arrow_frames > 0)
		{
			const int32_t arrow_frames_max = 40, arrow_freeze_frames_max = 20;

			draw_arrow(arrow_frames < arrow_frames_max ? (float)arrow_frames / (float)arrow_frames_max : 1.0);

			if (arrow_frames++ == arrow_frames_max + arrow_freeze_frames_max)
				arrow_frames = 0;
		}
		// menu
		if (menu_active_page != -1)
			draw_menu();
		else if (panel_active)
			draw_panel();
	}
	// GTA SA: The Definitive Edition (libUE4.so). Coordinates are fractions of the render size (any resolution).
	// Panels go through CSprite2d::DrawRect(FVector4), which DE draws below CFont text; other shapes
	// (arrows) through Draw2DPolygon, which DE draws above text, so only shapes behind text use DrawRect.
	namespace de
	{
		struct CRGBA { uint8_t r, g, b, a; };
		void (*Draw2DPolygon)(float, float, float, float, float, float, float, float, const CRGBA *);
		void (*DrawRect4)(const float *, const CRGBA *);
		void (*SetFontStyle)(uint8_t);
		void (*PrintString)(float, float, uint16_t *);
		float (*GetStringWidth)(uint16_t *, uint8_t, uint8_t);
		// DE draws UE4 fonts, measures with legacy SA metrics: drawn/legacy width per style. Body text at 0.6x of its legacy SA font metrics (measured: "carspawn" 98/162, "cheats" 62/104)
		const float kBodyFontWidth = 0.6f;
		const float kTitleFontWidth = 1.39f; // "CLEO MENU" 256/184
		int (*ScreenWidth)(), (*ScreenHeight)();
		uint8_t *Details; // CFont::Details, 0x40 bytes, SA mobile layout
		void (*CHud__Draw_)();
		void poll_touch();
		void (*RenderFontBuffer)();
		void CHud__Draw()
		{
			CHud__Draw_();
			poll_touch();
			// DE applies CFont::Details when it flushes buffered text: flush the game's text under its own
			// settings, CLEO's under CLEO's, then restore the game's
			uint8_t saved[0x40];
			memcpy(saved, Details, sizeof saved);
			RenderFontBuffer();
			on_draw();
			RenderFontBuffer();
			memcpy(Details, saved, sizeof saved);
		}
		float trans_x(float x) { return x * ScreenWidth(); }
		float trans_y(float y) { return y * ScreenHeight(); }
		void _draw_poly(float tlx, float tly, float trx, float try_, float blx, float bly, float brx, float bry, uint8_t *rgba)
		{
			const CRGBA *c = (const CRGBA *)rgba;
			if (tly == try_ && bly == bry && tlx == blx && trx == brx) // axis-aligned rect: may sit behind text
			{
				float r[4] = { trans_x(tlx), trans_y(tly), trans_x(trx), trans_y(bly) };
				DrawRect4(r, c);
			} else
				Draw2DPolygon(trans_x(tlx), trans_y(tly), trans_x(trx), trans_y(try_), trans_x(blx), trans_y(bly), trans_x(brx), trans_y(bry), c);
		}
		void _print_string(uint16_t *str, float x, float y, eAlign align, float scalex, float scaley, uint8_t *rgba, eStyle style)
		{
			// DE applies the centring flags when it flushes text later in the frame, so CLEO left-justifies and offsets
			Details[0x19] = Details[0x1A] = 0;    // m_bCentre, m_bRightJustify
			Details[0x18] = 1;                     // m_bLeftJustify
			switch (style)
			{
			case eStyleClassic: SetFontStyle(3); scalex *= 1.9; scaley *= 1.5; break;
			case eStyleSimple: SetFontStyle(1); scalex *= 1.4; scaley *= 1.3; break;
			case eStyleSpecial: SetFontStyle(0); break;
			}
			// SA mobile font scales are for a 448-high screen; DE scales text by the render height the same way
			*(float *)(Details + 0x04) = scalex;
			*(float *)(Details + 0x08) = scaley;
			memcpy(Details, rgba, 4);              // m_dwColor
			Details[0x1B] = 0;                     // m_bBackground
			Details[0x1F] = 0;                     // m_bFlash
			Details[0x1D] = 1;                     // m_bProp
			*(float *)(Details + 0x20) = 255.0f;   // m_fAlpha: DE draws nothing at 0
			*(float *)(Details + 0x28) = trans_x(1.0f);  // m_fWrapx
			*(float *)(Details + 0x2C) = trans_x(1.0f);  // m_fCentreSize
			Details[0x36] = 1;                     // m_bDropShadowPosition
			uint32_t drop = 0xFF000000;
			memcpy(Details + 0x37, &drop, 4);      // m_dwDropColor
			Details[0x3B] = Details[0x3C] = 0;     // edges
			float px = trans_x(x);
			if (align != eAlignLeft)
			{
				float w = GetStringWidth(str, 1, 0) * (style == eStyleClassic ? kTitleFontWidth : style == eStyleSimple ? kBodyFontWidth : 1.0f);
				px -= align == eAlignCenter ? w / 2 : w;
			}
			PrintString(px, trans_y(y), str);
		}
		bool init()
		{
			Draw2DPolygon = (decltype(Draw2DPolygon))core::getsym("_ZN9CSprite2d13Draw2DPolygonEffffffffRK5CRGBA");
			DrawRect4 = (decltype(DrawRect4))core::getsym("_ZN9CSprite2d8DrawRectERK8FVector4RK5CRGBA");
			SetFontStyle = (decltype(SetFontStyle))core::getsym("_ZN5CFont12SetFontStyleEh");
			PrintString = (decltype(PrintString))core::getsym("_ZN5CFont11PrintStringEffPt");
			RenderFontBuffer = (decltype(RenderFontBuffer))core::getsym("_ZN5CFont16RenderFontBufferEv");
			GetStringWidth = (decltype(GetStringWidth))core::getsym("_ZN5CFont14GetStringWidthEPthh");
			ScreenWidth = (decltype(ScreenWidth))core::getsym("_Z17OS_ScreenGetWidthv");
			ScreenHeight = (decltype(ScreenHeight))core::getsym("_Z18OS_ScreenGetHeightv");
			Details = (uint8_t *)core::getsym("_ZN5CFont7DetailsE");
			void *hud = core::getsym("_ZN4CHud4DrawEv");
			if (!(Draw2DPolygon && DrawRect4 && SetFontStyle && PrintString && RenderFontBuffer && GetStringWidth && ScreenWidth && ScreenHeight && Details && hud))
				return false;
			core::hook(hud, (void *)CHud__Draw, (void **)&CHud__Draw_);
			draw_poly = _draw_poly;
			print_string = _print_string;
			uint8_t color[4] = { 40, 235, 24, 255 };
			memcpy(menu_selected_item_font_color, color, sizeof(color));
			uint8_t arrow_color[4] = { 55, 127, 175, 100 };
			memcpy(menu_arrow_color, arrow_color, sizeof(arrow_color));
			menu_arrow_selected_alpha = 180;
			return true;
		}
	}

	// feeds CLEO touch events from the game's pointer state (DE has no AND_TouchEvent): pointers 0 and 1
	namespace de
	{
		int (*GetCoords)(int, float *, float *);
		struct { bool down; float x, y; } ptr_state[2];
		void poll_touch()
		{
			uint32_t w = ScreenWidth(), h = ScreenHeight();
			for (int i = 0; i < 2; i++)
			{
				float x = 0, y = 0;
				bool down = GetCoords(i, &x, &y) != 0;
				if (down)
				{
					touch::touch_event(ptr_state[i].down ? TOUCH_HOLD : TOUCH_DOWN, i, x, y, w, h);
					ptr_state[i].x = x;
					ptr_state[i].y = y;
				} else if (ptr_state[i].down)
					touch::touch_event(TOUCH_UP, i, ptr_state[i].x, ptr_state[i].y, w, h);
				ptr_state[i].down = down;
			}
		}
	}

	bool init()
	{
		de::GetCoords = (decltype(de::GetCoords))core::getsym("_Z25LIB_PointerGetCoordinatesiPfS_");
		return de::GetCoords && de::init();
	}
}
