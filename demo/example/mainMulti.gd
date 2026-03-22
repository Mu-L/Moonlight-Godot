extends Node

class MoonlightInstance:
    var index: int = 0
    var moonlight_config_manager: MoonlightConfigManager
    var moonlight_computer_manager: MoonlightComputerManager
    var moonlightstreamcore: MoonlightStreamCore
    var audio_player: AudioStreamPlayer
    var texture_rect: TextureRect
    var custom_config_path: String

var instances: Array[MoonlightInstance] = []
var current_instance: MoonlightInstance = null

@onready var moonlight_requester = MoonlightRequester.new()

var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var is_fullscreen: bool = false
var vk_modifiers: int = 0

func _ready() -> void:
    _setup_virtual_keyboard()
    _add_instance() # add first instance

func _add_instance() -> void:
    var inst = MoonlightInstance.new()
    inst.index = instances.size()
    if inst.index == 0:
        inst.custom_config_path = ""
    else:
        inst.custom_config_path = "user://addons/moonlight-godot/multiconfig/config%s.ini" % inst.index
        
    inst.moonlight_config_manager = MoonlightConfigManager.new()
    inst.moonlight_computer_manager = MoonlightComputerManager.new()
    inst.moonlightstreamcore = MoonlightStreamCore.new()
    
    if inst.custom_config_path != "":
        inst.moonlight_config_manager.config_path = inst.custom_config_path
    inst.moonlight_computer_manager.set_config_manager(inst.moonlight_config_manager)
    inst.moonlightstreamcore.set_config_manager(inst.moonlight_config_manager)
    
    inst.moonlight_computer_manager.pair_completed.connect(on_pair_complete)
    inst.moonlightstreamcore.connection_started.connect(func(): print("[Instance %s] Connect Successfully" % inst.index))
    inst.moonlightstreamcore.connection_terminated.connect(func(_err, msg): push_error("[Instance %s] " % inst.index, msg))
    
    # Create nodes
    var audio_p = AudioStreamPlayer.new()
    audio_p.autoplay = true
    $ScrollContainer/GridContainer.add_child(audio_p)
    inst.audio_player = audio_p
    
    var tex_r = TextureRect.new()
    tex_r.custom_minimum_size = Vector2(800, 450)
    tex_r.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
    tex_r.gui_input.connect(func(event): _on_instance_screen_gui_input(event, inst))
    $ScrollContainer/GridContainer/ScreensGrid.add_child(tex_r)
    inst.texture_rect = tex_r
    
    instances.append(inst)
    
    var opt_btn = $ScrollContainer/GridContainer/InstanceOption
    opt_btn.add_item("Instance " + str(inst.index))
    
    if current_instance == null:
        _select_instance(0)

func _on_add_instance_btn_pressed() -> void:
    _add_instance()

func _on_instance_option_item_selected(index: int) -> void:
    _select_instance(index)

func _select_instance(index: int) -> void:
    if index >= 0 and index < instances.size():
        current_instance = instances[index]
        var opt_btn = $ScrollContainer/GridContainer/InstanceOption
        opt_btn.selected = index
        # Update UI info based on current instance, setup ip if available
        var hosts = current_instance.moonlight_config_manager.get_hosts()
        if hosts.size() > 0:
            $ScrollContainer/GridContainer/LineEdit.text = hosts[0].localaddress

func on_pair_complete(success, message):
    if success: print("[Moonlight-Godot] ", message)
    else: push_error("[Moonlight-Godot] ", message)

# Helper for delegating to current instance core objects
func _moonlight() -> MoonlightStreamCore:
    if current_instance: return current_instance.moonlightstreamcore
    return null

func _computer() -> MoonlightComputerManager:
    if current_instance: return current_instance.moonlight_computer_manager
    return null

func _config() -> MoonlightConfigManager:
    if current_instance: return current_instance.moonlight_config_manager
    return null

# ... keeping existing UI tests functioning over current ...

func test_http_requeset() -> void:
    var url = "http://httpbin.org/uuid"
    moonlight_requester.request(url, "GET", PackedByteArray(), {}, {}, Callable(self , "_on_baidu_request_completed"))

func _on_baidu_request_completed(response_code: int, response_body: PackedByteArray, response_headers: Dictionary, error_text: String) -> void:
    if error_text.is_empty():
        print("Success: ", response_code, " ", response_body.get_string_from_utf8())
    else:
        print("Fail: ", error_text)

func test_cert_create() -> void:
    if _config(): print(_config().get_client_keys())

func test_add_host_info() -> void:
    if _config():
        print("current hosts:\n", _config().get_hosts(), "\n")

func test_remove_host_info() -> void:
    if _config(): _config().remove_host(1)

func test_start_pair() -> void:
    if _computer():
        var pin = _computer().start_pair($ScrollContainer/GridContainer/LineEdit.text)
        print("pin:", pin)

func test_cancel_pair() -> void:
    if _computer(): _computer().unpair(1)

func test_get_applist() -> void:
    if _computer(): _computer().get_app_list(1)

func test_get_app_texture() -> void:
    if _config() and _computer():
        _config().load_config()
        var applist = _config().get_apps(1)
        if applist.size() > 0:
            _computer().get_app_cover(1, applist[0]["id"], func(texture_return):
                $ScrollContainer/GridContainer/TextureRect.texture = texture_return
            )

func test_connect_to_server() -> void:
    if _computer(): _computer().connect_to_computer($ScrollContainer/GridContainer/LineEdit.text, 47989, func(info): print(info))

var current_stream_config: MoonlightStreamConfigurationResource = null

func test_establish_stream() -> void:
    if not _moonlight() or not current_instance: return
    
    var cfg = MoonlightStreamConfigurationResource.new()
    cfg.set_width(1920)
    cfg.set_height(1080)
    cfg.set_fps(60)
    cfg.set_bitrate(int($ScrollContainer/GridContainer/LineEdit2.text) * 10000)
    current_stream_config = cfg

    video_enabled = $ScrollContainer/GridContainer/ToggleVideoButton.button_pressed
    audio_enabled = $ScrollContainer/GridContainer/ToggleAudioButton.button_pressed

    var add_opts = MoonlightAdditionalStreamOptions.new()
    add_opts.set_video_codec($ScrollContainer/GridContainer/OptionButton.get_selected_id())
    add_opts.set_disable_hw_acceleration(not $ScrollContainer/GridContainer/CheckButton.button_pressed)
    add_opts.set_disable_video(not video_enabled)
    add_opts.set_disable_audio(not audio_enabled)

    if video_enabled:
        _moonlight().set_render_target(current_instance.texture_rect)
        
    if audio_enabled:
        current_instance.audio_player.stream = _moonlight().get_audio_stream()

    _moonlight().start_play_stream(1, 1191261554, cfg, add_opts)
    await get_tree().create_timer(1).timeout
    if audio_enabled:
        current_instance.audio_player.play()

func test_stop_stream() -> void:
    if _computer() and _moonlight():
        _computer().stop_stream(1, func(info):
            print(info)
            _moonlight().stop_play_stream()
            _moonlight().reset_audio_stream()
            _moonlight().reset_render_target()
        )

func test_pause_streram() -> void:
    if _moonlight():
        _moonlight().stop_play_stream()
        _moonlight().reset_audio_stream()
        _moonlight().reset_render_target()

func view_data() -> void:
    $ScrollContainer/FileDialog.visible = true

func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if _config() and not toggled_on:
        _config().update_host(1, {"localaddress": $ScrollContainer/GridContainer/LineEdit.text})

func test_toggle_audio_bypass() -> void:
    if _moonlight():
        if _moonlight().is_native_audio_bypass_running():
            _moonlight().stop_native_audio_bypass()
            $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: OFF"
        else:
            var ok = _moonlight().start_native_audio_bypass()
            $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON" if ok else "Audio Bypass: FAILED"

func test_toggle_audio_pause() -> void:
    if _moonlight():
        if not _moonlight().is_native_audio_bypass_running(): return
        if _moonlight().is_native_audio_bypass_paused():
            _moonlight().resume_native_audio_bypass()
            $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Pause"
        else:
            _moonlight().pause_native_audio_bypass()
            $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Resume"

func _on_instance_screen_gui_input(event: InputEvent, inst: MoonlightInstance) -> void:
    if not input_enabled or not inst.moonlightstreamcore: return
    
    var screen_width = int(inst.texture_rect.size.x)
    var screen_height = int(inst.texture_rect.size.y)
    var mc = inst.moonlightstreamcore

    if event is InputEventMouseMotion:
        mc.send_mouse_position_event(int(event.position.x), int(event.position.y), screen_width, screen_height)
    elif event is InputEventMouseButton:
        var action = MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_PRESS if event.pressed else MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_RELEASE
        var button = 0
        match event.button_index:
            MOUSE_BUTTON_LEFT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_LEFT
            MOUSE_BUTTON_RIGHT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_RIGHT
            MOUSE_BUTTON_MIDDLE: button = MoonlightInput.MouseButton.MOUSE_BUTTON_MIDDLE
            MOUSE_BUTTON_WHEEL_UP:
                mc.send_scroll_event(1)
                return
            MOUSE_BUTTON_WHEEL_DOWN:
                mc.send_scroll_event(-1)
                return
            MOUSE_BUTTON_XBUTTON1: button = MoonlightInput.MouseButton.MOUSE_BUTTON_X1
            MOUSE_BUTTON_XBUTTON2: button = MoonlightInput.MouseButton.MOUSE_BUTTON_X2
        if button != 0:
            mc.send_mouse_button_event(action, button)
    elif event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.shift_pressed: modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.ctrl_pressed: modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.alt_pressed: modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed: modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        # The backend now handles mapping Godot keycodes to standard HID keycodes.
        # We can pass the Godot keycode directly without any manual conversion here.
        var keycode = event.keycode
        mc.send_keyboard_event(keycode, action, modifiers)

func _on_check_button_2_toggled(toggled_on: bool) -> void:
    if current_instance and current_instance.audio_player:
        current_instance.audio_player.playing = toggled_on

func _on_toggle_input_pressed() -> void:
    input_enabled = $ScrollContainer/GridContainer/ToggleInputButton.button_pressed

func test_toggle_keyboard() -> void:
    $KeyboardLayer.visible = not $KeyboardLayer.visible

func _setup_virtual_keyboard() -> void:
    pass

func _on_vk_modifier_toggled(keycode: int, pressed: bool) -> void:
    pass

func _on_vk_key_event(keycode: int, pressed: bool) -> void:
    if not input_enabled or not _moonlight(): return
    var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
    _moonlight().send_keyboard_event(keycode, action, vk_modifiers)

func _on_fullscreen_pressed() -> void:
    is_fullscreen = true
    $FullscreenLayer.visible = true
    
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen
    
    var stream_width = 1920.0
    var stream_height = 1080.0
    if current_stream_config != null:
        stream_width = float(current_stream_config.get_width())
        stream_height = float(current_stream_config.get_height())
        
    var stream_ratio = stream_width / stream_height
    
    var window_size = get_viewport().get_visible_rect().size
    var window_ratio = window_size.x / window_size.y
    
    var target_size = Vector2()
    if window_ratio > stream_ratio:
        target_size.y = window_size.y
        target_size.x = target_size.y * stream_ratio
    else:
        target_size.x = window_size.x
        target_size.y = target_size.x / stream_ratio
        
    screen.custom_minimum_size = target_size

    if video_enabled and _moonlight():
        _moonlight().set_render_target(screen)

func _on_exit_fullscreen_pressed() -> void:
    is_fullscreen = false
    $FullscreenLayer.visible = false
    
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen
    screen.custom_minimum_size = Vector2(0, 0)
    
    if video_enabled and _moonlight() and current_instance:
        _moonlight().set_render_target(current_instance.texture_rect)

func _on_screen_gui_input(event: InputEvent) -> void:
    if is_fullscreen and current_instance:
        # Re-route fullscreen input to current instance logic, adapting rect
        var fw = int($FullscreenLayer/CenterContainer/FullscreenScreen.size.x)
        var fh = int($FullscreenLayer/CenterContainer/FullscreenScreen.size.y)
        var temp_r = current_instance.texture_rect.size
        current_instance.texture_rect.size = Vector2(fw, fh)
        _on_instance_screen_gui_input(event, current_instance)
        current_instance.texture_rect.size = temp_r
