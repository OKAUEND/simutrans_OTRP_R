/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#include "siminteraction.h"

#include "simversion.h"
#include "dataobj/environment.h"
#include "gui/gui_frame.h"
#include "gui/help_frame.h"
#include "network/network_cmd_ingame.h"
#include "dataobj/scenario.h"
#include "simevent.h"
#include "utils/simrandom.h"
#include "tool/simmenu.h"
#include "player/simplay.h"
#include "simsound.h"
#include "sys/simsys.h"
#include "simticker.h"
#include "gui/simwin.h"
#include "world/simworld.h"
#include "descriptor/sound_desc.h"
#include "obj/zeiger.h"
#include "display/viewport.h"


karte_ptr_t interaction_t::world;

void interaction_t::move_view( const event_t &ev )
{
	koord new_ij = viewport->get_world_position();

	sint16 new_xoff = viewport->get_x_off() - (ev.mx - ev.cx) * env_t::scroll_multi;
	sint16 new_yoff = viewport->get_y_off() - (ev.my - ev.cy) * env_t::scroll_multi;

	// this sets the new position and mark screen dirty
	// => with next refresh we will be at a new location
	viewport->change_world_position( new_ij, new_xoff, new_yoff );

	// move the mouse pointer back to starting location => infinite mouse movement
	if ((ev.mx - ev.cx) != 0 || (ev.my - ev.cy) != 0) {
		if(!env_t::scroll_infinite  ||  !move_pointer(ev.cx, ev.cy)) {
			// fails in finger mode
			change_drag_start(ev.mx - ev.cx, ev.my - ev.cy);
		}
	}
}


void interaction_t::move_cursor( const event_t &ev )
{
	zeiger_t *zeiger = world->get_zeiger();

	if(! zeiger ) {
		// No cursor to move, exit
		return;
	}

	static int mb_alt=0;

	tool_t *tool = world->get_tool(world->get_active_player_nr());

	const koord3d pos = viewport->get_new_cursor_position(scr_coord(ev.mx,ev.my), tool->is_grid_tool());

	if( pos == koord3d::invalid ) {
		zeiger->change_pos(pos);
		return;
	}

	// move cursor
	const koord3d prev_pos = zeiger->get_pos();
	if(  (prev_pos != pos ||  ev.button_state != mb_alt)  ) {

		mb_alt = ev.button_state;

		zeiger->change_pos(pos);

		if (!tool->move_has_effects()) {
			is_dragging = false;
		}
		else {
			tool->flags = (event_get_last_control_shift() ^ tool_t::control_invert) | tool_t::WFL_LOCAL;
			if(tool->check_pos( world->get_active_player(), zeiger->get_pos() )==NULL) {
				if(  ev.button_state == 0  ) {
					is_dragging = false;
				}
				else if(ev.ev_class==EVENT_DRAG) {
					if(!is_dragging  &&  prev_pos != koord3d::invalid  &&  tool->check_pos( world->get_active_player(), prev_pos )==NULL) {
						is_dragging = true;
					}
				}
				if (is_dragging) {
					world->set_deferred_move_to(pos, tool->flags);
				}
			}
			tool->flags = 0;
		}

		if(  (ev.button_state&7)==0  ) {
			// time, since mouse got here
			world->set_mouse_rest_time(dr_time());
			world->set_sound_wait_time(AMBIENT_SOUND_INTERVALL); // 13s no movement: play sound
		}
	}
}


void interaction_t::interactive_event( const event_t &ev )
{
	if(ev.ev_class == EVENT_KEYBOARD) {
		DBG_MESSAGE("interaction_t::interactive_event()","Keyboard event with code %d '%c'", ev.ev_code, (ev.ev_code>=32  &&  ev.ev_code<=126) ? ev.ev_code : '?' );

		switch(ev.ev_code) {

			// cursor movements
			case SIM_KEY_UPRIGHT:
				viewport->change_world_position(viewport->get_world_position() + koord::north);
				world->set_dirty();
				break;
			case SIM_KEY_DOWNLEFT:
				viewport->change_world_position(viewport->get_world_position() + koord::south);
				world->set_dirty();
				break;
			case SIM_KEY_UPLEFT:
				viewport->change_world_position(viewport->get_world_position() + koord::west);
				world->set_dirty();
				break;
			case SIM_KEY_DOWNRIGHT:
				viewport->change_world_position(viewport->get_world_position() + koord::east);
				world->set_dirty();
				break;
			case SIM_KEY_RIGHT:
				viewport->change_world_position(viewport->get_world_position() + koord(+1,-1));
				world->set_dirty();
				break;
			case SIM_KEY_DOWN:
				viewport->change_world_position(viewport->get_world_position() + koord(+1,+1));
				world->set_dirty();
				break;
			case SIM_KEY_UP:
				viewport->change_world_position(viewport->get_world_position() + koord(-1,-1));
				world->set_dirty();
				break;
			case SIM_KEY_LEFT:
				viewport->change_world_position(viewport->get_world_position() + koord(-1,+1));
				world->set_dirty();
				break;

			// closing windows
			case 27:
			case 127:
				if( !IS_CONTROL_PRESSED( &ev ) && !IS_SHIFT_PRESSED( &ev ) ) {
					// close topmost win
					destroy_win( win_get_top() );
				}
				break;

			case SIM_KEY_F1:
				if(  gui_frame_t *win = win_get_top()  ) {
					if(  const char *helpfile = win->get_help_filename()  ) {
						help_frame_t::open_help_on( helpfile );
						break;
					}
				}
				world->set_tool( tool_t::dialog_tool[DIALOG_HELP], world->get_active_player() );
				break;

			// just ignore the key
			case 0:
				break;

			// distinguish between backspace and ctrl-H (both keycode==8), and enter and ctrl-M (both keycode==13)
			case 8:
			case 13:
				if(  !IS_CONTROL_PRESSED(&ev)  &&  !IS_SHIFT_PRESSED(&ev)  ) {
					// Control is _not_ pressed => Backspace or Enter pressed.
					if(  ev.ev_code == 8  ) {
						// Backspace
						sound_play(SFX_SELECT,255,TOOL_SOUND);
						destroy_all_win(false);
					}
					// Ignore Enter and Backspace but not Ctrl-H and Ctrl-M
					break;
				}
				/* FALLTHROUGH */

			default:
				{
					bool ok=false;
					for(tool_t* const i : tool_t::char_to_tool) {
						if(  i->command_key == ev.ev_code  ) {
							if(  i->command_flags == 0  ||  (ev.ev_key_mod & (SIM_MOD_SHIFT|SIM_MOD_CTRL)) == i->command_flags  ) {
								world->set_tool(i, world->get_active_player());
								ok = true;
								break;
							}
						}
					}
#ifdef STEAM_BUILT
					// Block F12 from bringing up Keyboard Help (for Steam Screenshot) - but still allow F12 to be used if defined in pakset
					if (ev.ev_code==SIM_KEY_F12) {
						ok=true;
					}
#endif
					if(!ok) {
						help_frame_t::open_help_on( "keys.txt" );
					}
				}
				break;
		}
	}

	if(  !is_dragging  &&  IS_LEFTRELEASE(&ev)  &&  ev.my < display_get_height() -16 -(TICKER_HEIGHT*ticker::empty())  ) {

		DBG_MESSAGE("interaction_t::interactive_event(event_t &ev)", "calling a tool");

		koord3d pos = world->get_zeiger()->get_pos();
		if(world->is_within_grid_limits(pos.get_2d())) {

			bool suspended = false; // true if execution was suspended, i.e. sent to server
			tool_t *tool = world->get_tool(world->get_active_player_nr());
			player_t *player = world->get_active_player();
			tool->flags = event_get_last_control_shift() ^ tool_t::control_invert;
			// first check for visibility etc (needs already right flags)
			const char *err = tool->check_pos( player, pos );
			if (err==NULL) {
				err = world->call_work_api(tool, player, pos, suspended, false);
			}
			if (!suspended) {
				// play sound / error message
				world->get_active_player()->tell_tool_result(tool, pos, err);

				// Check if we need to update pointer(zeiger) position.
				if( err == NULL  &&  tool->update_pos_after_use() ) {
					// Cursor might need movement (screen has changed, we get a new one under the mouse pointer)
					const koord3d pos_new = viewport->get_new_cursor_position(scr_coord(ev.mx,ev.my), tool->is_grid_tool());
					world->get_zeiger()->set_pos(pos_new);
				}
			}
			tool->flags = 0;
		}
	}

	// mouse wheel scrolled -> rezoom
	if (ev.ev_class == EVENT_CLICK) {

		// first, we need to check cursor is valid, won't zoom otherwise

		const koord3d cursor_pos = world->get_zeiger()->get_pos();

		if( cursor_pos == koord3d::invalid) {
			//ignore event
			return;
		}

		bool zoom_successful = false;

		// store old screen position of centered tile
		scr_coord s = viewport->get_screen_coord(cursor_pos, koord(0,0));

		if(ev.ev_code==MOUSE_WHEELUP) {
			if(win_change_zoom_factor(true)) {
				zoom_successful = true;
			}
		}
		else if(ev.ev_code==MOUSE_WHEELDOWN) {
			if(win_change_zoom_factor(false)) {
				zoom_successful = true;
			}
		}

		// zoom can fail if we are max zoomed in/out, so:
		if (zoom_successful) {
			// calculate offsets such that tile under cursor is still on the same screen position
			viewport->change_world_position(cursor_pos, koord(0,0), s);

			//and move cursor to the new position under the mouse
			move_cursor(ev);

			world->set_dirty();
		}
	}
}


bool interaction_t::process_event( event_t &ev )
{
	if(ev.ev_class==EVENT_SYSTEM  &&  ev.ev_code==SYSTEM_QUIT) {
		// since we run in a sync_step, quitting may be needed to be delagated to a tool
		if(  (LOAD_RANDOM | MAP_CREATE_RANDOM | MODAL_RANDOM) & get_random_mode()  ) {
			// next sync step would take tool long
			world->stop(true);
		}
		else {
			// we call the proper tool for quitting
			world->set_tool(tool_t::simple_tool[TOOL_QUIT], NULL);
		}
		return true;
	}

	if(ev.ev_class==IGNORE_EVENT) {
		// ignore it
		return false;
	}

	DBG_DEBUG4("interaction_t::process_event", "calling check_pos_win");
	if(check_pos_win(&ev)){
		// The event is shallowed by the GUI, next.
		return false;
	}

	DBG_DEBUG4("interaction_t::process_event", "after check_pos_win");

	// Handle map drag with right-click
	if(IS_RIGHTCLICK(&ev)) {
		display_show_pointer(false);
	}
	else if(IS_RIGHTRELEASE(&ev)) {
		display_show_pointer(true);
	}
	else if(IS_RIGHTDRAG(&ev)) {
		// unset following
		world->get_viewport()->set_follow_convoi( convoihandle_t() );
		catch_dragging();
		move_view(ev);
	}
	else if(  IS_LEFTDRAG(&ev)  &&  IS_LEFT_BUTTON_PRESSED(&ev)  &&  (is_world_dragging  ||  (!world->get_tool(world->get_active_player_nr())->move_has_effects()  &&  !IS_CONTROL_PRESSED(&ev))  )  ) {
		/* ok, we have a general tool selected, and we have a left drag or left release event with an actual difference
		 * => move the map, if we are beyond a threshold */
		if(  is_world_dragging  ||  abs(ev.cx-ev.mx)+abs(ev.cy-ev.my)>=max(1,(env_t::scroll_threshold* get_tile_raster_width())/get_base_tile_raster_width())  ) {
			if (!is_world_dragging) {
				display_show_pointer(false);
				is_world_dragging = true;
			}
			world->get_viewport()->set_follow_convoi(convoihandle_t());
			catch_dragging();
			move_view(ev);
			ev.ev_code = IGNORE_EVENT;
		}
	}

	if( !IS_LEFT_BUTTON_PRESSED(&ev)  &&  is_world_dragging  ) {
		// show the mouse and swallow this event if we were dragging before
		ev.ev_code = IGNORE_EVENT;
		display_show_pointer(true);
		is_world_dragging = false;
	}


	DBG_DEBUG4("interaction_t::process_event", "check if cursor needs movement");


	if( (ev.ev_class==EVENT_DRAG  &&  ev.ev_code==MOUSE_LEFTBUTTON)  ||  (ev.button_state==0  &&  ev.ev_class==EVENT_MOVE)  ||  ev.ev_class==EVENT_RELEASE) {
		move_cursor(ev);
	}

	DBG_DEBUG4("interaction_t::process_event", "calling interactive_event");

	interactive_event(ev);

	DBG_DEBUG4("interaction_t::process_event", "end of event handling");

	return false;
}


void interaction_t::check_events()
{
	event_t ev;

	bool const set_rmode_interactive = env_t::networkmode;
	if (set_rmode_interactive) {
		set_random_mode( INTERACTIVE_RANDOM );
	}

	win_poll_event(&ev);

	event_t deferred_ev;
	deferred_ev.ev_class = EVENT_NONE;

	while(  ev.ev_class != EVENT_NONE ) {

		DBG_DEBUG4("interaction_t::check_events", "called win_poll_event");

		if (ev.ev_class == EVENT_DRAG) {
			// defer processing, since there might be many triggered at once
			// Otherwise mark tiles could be alled twice duing one step
			deferred_ev = ev;
		}
		else {
			// still one drag left in queue?
			if (deferred_ev.ev_class == EVENT_DRAG) {
				// do this first
				process_event(deferred_ev);
				deferred_ev.ev_class = EVENT_NONE;
			}

			if (process_event(ev)) {
				// We have been asked to stop processing, exit.
				// but clear random mode flag first
				if(  set_rmode_interactive  ) {
					clear_random_mode( INTERACTIVE_RANDOM );
				}
				return;
			}
		}

		win_poll_event(&ev);
	}

	if (deferred_ev.ev_class == EVENT_DRAG) {
		// process pending drag events
		process_event(deferred_ev);
		deferred_ev.ev_class = EVENT_NONE;
	}

	if(  set_rmode_interactive  ) {
		clear_random_mode( INTERACTIVE_RANDOM );
	}
}


interaction_t::interaction_t(viewport_t *viewport) :
	viewport(viewport),
	is_dragging(false),
	is_world_dragging(false)
{
	assert(viewport);
}
