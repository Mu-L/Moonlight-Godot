// 封装 Limelight 输入 API，供 Godot 调用
#include "stream_core.h"
#include "stream_core_input_enum.h"

using namespace godot;

int MoonlightStreamCore::send_mouse_move_event(short delta_x, short delta_y) {
	return LiSendMouseMoveEvent(delta_x, delta_y);
}

int MoonlightStreamCore::send_mouse_position_event(short x, short y, int reference_width, int reference_height) {
	return LiSendMousePositionEvent(x, y, reference_width, reference_height);
}

int MoonlightStreamCore::send_mouse_move_as_mouse_position_event(short delta_x, short delta_y, int reference_width, int reference_height) {
	return LiSendMouseMoveAsMousePositionEvent(delta_x, delta_y, reference_width, reference_height);
}

int MoonlightStreamCore::send_touch_event(int event_type, int pointer_id, float x, float y, float pressure_or_distance,
		float contact_area_major, float contact_area_minor, int rotation) {
	return LiSendTouchEvent((uint8_t)event_type, (uint32_t)pointer_id, x, y, pressure_or_distance, contact_area_major, contact_area_minor, (uint16_t)rotation);
}

int MoonlightStreamCore::send_pen_event(int event_type, int tool_type, int pen_buttons,
		float x, float y, float pressure_or_distance,
		float contact_area_major, float contact_area_minor,
		int rotation, int tilt) {
	return LiSendPenEvent((uint8_t)event_type, (uint8_t)tool_type, (uint8_t)pen_buttons, x, y, pressure_or_distance, contact_area_major, contact_area_minor, (uint16_t)rotation, (uint8_t)tilt);
}

int MoonlightStreamCore::send_mouse_button_event(int action, int button) {
	return LiSendMouseButtonEvent((char)action, button);
}

int MoonlightStreamCore::send_keyboard_event(int godot_key, int key_action, int modifiers) {
	short vkey = (short)MoonlightInput::godot_to_virtual_key(godot_key);
	if (vkey == 0)
		return 0; // Ignore unmapped keys
	return LiSendKeyboardEvent(vkey, (char)key_action, (char)modifiers);
}

int MoonlightStreamCore::send_keyboard_event2(int godot_key, int key_action, int modifiers, int flags) {
	short vkey = (short)MoonlightInput::godot_to_virtual_key(godot_key);
	if (vkey == 0)
		return 0;
	return LiSendKeyboardEvent2(vkey, (char)key_action, (char)modifiers, (char)flags);
}

int MoonlightStreamCore::send_utf8_text_event(const String &text) {
	CharString cs = text.utf8();
	return LiSendUtf8TextEvent(cs.get_data(), (unsigned int)cs.length());
}

int MoonlightStreamCore::send_controller_event(int button_flags, int left_trigger, int right_trigger,
		short left_stick_x, short left_stick_y, short right_stick_x, short right_stick_y) {
	return LiSendControllerEvent(button_flags, (unsigned char)left_trigger, (unsigned char)right_trigger, left_stick_x, left_stick_y, right_stick_x, right_stick_y);
}

int MoonlightStreamCore::send_multi_controller_event(int controller_number, int active_gamepad_mask,
		int button_flags, int left_trigger, int right_trigger,
		short left_stick_x, short left_stick_y, short right_stick_x, short right_stick_y) {
	return LiSendMultiControllerEvent((short)controller_number, (short)active_gamepad_mask, button_flags, (unsigned char)left_trigger, (unsigned char)right_trigger, left_stick_x, left_stick_y, right_stick_x, right_stick_y);
}

int MoonlightStreamCore::send_controller_arrival_event(int controller_number, int active_gamepad_mask, int type,
		uint32_t supported_button_flags, int capabilities) {
	return LiSendControllerArrivalEvent((uint8_t)controller_number, (uint16_t)active_gamepad_mask, (uint8_t)type, supported_button_flags, (uint16_t)capabilities);
}

int MoonlightStreamCore::send_controller_touch_event(int controller_number, int event_type, int pointer_id, float x, float y, float pressure) {
	return LiSendControllerTouchEvent((uint8_t)controller_number, (uint8_t)event_type, (uint32_t)pointer_id, x, y, pressure);
}

int MoonlightStreamCore::send_controller_motion_event(int controller_number, int motion_type, float x, float y, float z) {
	return LiSendControllerMotionEvent((uint8_t)controller_number, (uint8_t)motion_type, x, y, z);
}

int MoonlightStreamCore::send_controller_battery_event(int controller_number, int battery_state, int battery_percentage) {
	return LiSendControllerBatteryEvent((uint8_t)controller_number, (uint8_t)battery_state, (uint8_t)battery_percentage);
}

int MoonlightStreamCore::send_scroll_event(int scroll_clicks) {
	return LiSendScrollEvent((signed char)scroll_clicks);
}

int MoonlightStreamCore::send_high_res_scroll_event(short scroll_amount) {
	return LiSendHighResScrollEvent(scroll_amount);
}

int MoonlightStreamCore::send_hscroll_event(int scroll_clicks) {
	return LiSendHScrollEvent((signed char)scroll_clicks);
}

int MoonlightStreamCore::send_high_res_hscroll_event(short scroll_amount) {
	return LiSendHighResHScrollEvent(scroll_amount);
}

uint32_t MoonlightStreamCore::get_host_feature_flags() {
	return LiGetHostFeatureFlags();
}
