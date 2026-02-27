extends Node

@onready var requester = Requester.new()
@onready var configmanager = ConfigManager.new()
@onready var computermamager = ComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var is_fullscreen: bool = false

func _ready() -> void:
    computermamager.pair_completed.connect(on_pair_complete)
    moonlightstreamcore.connection_started.connect(func():print("[Moonlight-Godot-MoonlightStreamCore]","Connect Successfully!"))
    moonlightstreamcore.connection_terminated.connect(func(_err,msg):push_error("[Moonlight-Godot-MoonlightStreamCore]",msg))
    @warning_ignore("standalone_ternary")
    moonlightstreamcore.hdr_mode_changed.connect(func(enable,data):print("[Moonlight-Godot-MoonlightStreamCore-HDR]",data) if enable else push_warning("[Moonlight-Godot-MoonlightStreamCore-HDR]",data))
    $ScrollContainer/GridContainer/LineEdit.text = configmanager.get_hosts()[0].localaddress
    # 初始化音频直通按钮文本
    if $ScrollContainer/GridContainer.has_node("AudioBypassButton"):
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON" if moonlightstreamcore.is_native_audio_bypass_running() else "Audio Bypass: OFF"
    if $ScrollContainer/GridContainer.has_node("AudioPauseButton"):
        var paused = moonlightstreamcore.is_native_audio_bypass_paused()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Resume" if paused else "Pause"
    
    if $ScrollContainer/GridContainer.has_node("FullscreenButton"):
        $ScrollContainer/GridContainer/FullscreenButton.pressed.connect(_on_fullscreen_pressed)
        $ScrollContainer/GridContainer/ToggleInputButton.pressed.connect(_on_toggle_input_pressed)
        $FullscreenLayer/ExitFullscreenButton.pressed.connect(_on_exit_fullscreen_pressed)
        $FullscreenLayer/CenterContainer/FullscreenScreen.gui_input.connect(_on_screen_gui_input)
    
    _setup_virtual_keyboard()

func _setup_virtual_keyboard() -> void:
    var row1 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row1
    var row2 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row2
    var row3 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row3
    var row4 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row4
    
    var keys_r1 = [
        {"text": "F1", "code": MoonlightInput.VK_F1}, {"text": "F2", "code": MoonlightInput.VK_F2}, {"text": "F3", "code": MoonlightInput.VK_F3},
        {"text": "F4", "code": MoonlightInput.VK_F4}, {"text": "F5", "code": MoonlightInput.VK_F5}, {"text": "F6", "code": MoonlightInput.VK_F6},
        {"text": "F7", "code": MoonlightInput.VK_F7}, {"text": "F8", "code": MoonlightInput.VK_F8}, {"text": "F9", "code": MoonlightInput.VK_F9},
        {"text": "F10", "code": MoonlightInput.VK_F10}, {"text": "F11", "code": MoonlightInput.VK_F11}, {"text": "F12", "code": MoonlightInput.VK_F12},
        {"text": "Hide", "code": -1}
    ]
    var keys_r2 = [
        {"text": "1", "code": MoonlightInput.VK_1}, {"text": "2", "code": MoonlightInput.VK_2}, {"text": "3", "code": MoonlightInput.VK_3},
        {"text": "4", "code": MoonlightInput.VK_4}, {"text": "5", "code": MoonlightInput.VK_5}, {"text": "6", "code": MoonlightInput.VK_6},
        {"text": "7", "code": MoonlightInput.VK_7}, {"text": "8", "code": MoonlightInput.VK_8}, {"text": "9", "code": MoonlightInput.VK_9},
        {"text": "0", "code": MoonlightInput.VK_0}
    ]
    var keys_r3 = [
        {"text": "A", "code": MoonlightInput.VK_A}, {"text": "C", "code": MoonlightInput.VK_C}, {"text": "V", "code": MoonlightInput.VK_V},
        {"text": "Z", "code": MoonlightInput.VK_Z}, {"text": "X", "code": MoonlightInput.VK_X},
        {"text": "? /", "code": MoonlightInput.VK_OEM_2}, {"text": "{ [", "code": MoonlightInput.VK_OEM_4}, {"text": "} ]", "code": MoonlightInput.VK_OEM_6}
    ]
    var keys_r4 = [
        {"text": "Ctrl", "code": MoonlightInput.VK_CONTROL, "toggle": true},
        {"text": "Shift", "code": MoonlightInput.VK_SHIFT, "toggle": true},
        {"text": "Alt", "code": MoonlightInput.VK_MENU, "toggle": true},
        {"text": "Meta", "code": MoonlightInput.VK_LWIN, "toggle": true}
    ]
    
    var create_btn = func(k: Dictionary, parent: Node):
        var btn = Button.new()
        btn.text = k["text"]
        btn.custom_minimum_size = Vector2(50, 50)
        btn.focus_mode = Control.FOCUS_NONE
        if k.get("toggle", false):
            btn.toggle_mode = true
            btn.toggled.connect(func(toggled_on): _on_vk_modifier_toggled(k["code"], toggled_on))
        elif k["code"] == -1:
            btn.pressed.connect(func(): $KeyboardLayer.visible = false)
        else:
            btn.button_down.connect(func(): _on_vk_key_event(k["code"], true))
            btn.button_up.connect(func(): _on_vk_key_event(k["code"], false))
        parent.add_child(btn)
        
    for k in keys_r1: create_btn.call(k, row1)
    for k in keys_r2: create_btn.call(k, row2)
    for k in keys_r3: create_btn.call(k, row3)
    for k in keys_r4: create_btn.call(k, row4)

var vk_modifiers: int = 0

func _on_vk_modifier_toggled(keycode: int, pressed: bool) -> void:
    var mod_bit = 0
    if keycode == MoonlightInput.VK_CONTROL: mod_bit = MoonlightInput.MODIFIER_CTRL_BIT
    elif keycode == MoonlightInput.VK_SHIFT: mod_bit = MoonlightInput.MODIFIER_SHIFT_BIT
    elif keycode == MoonlightInput.VK_MENU: mod_bit = MoonlightInput.MODIFIER_ALT_BIT
    elif keycode == MoonlightInput.VK_LWIN: mod_bit = MoonlightInput.MODIFIER_META_BIT
    
    if pressed:
        vk_modifiers |= mod_bit
    else:
        vk_modifiers &= ~mod_bit

func _on_vk_key_event(keycode: int, pressed: bool) -> void:
    if not input_enabled:
        return
    var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
    moonlightstreamcore.send_keyboard_event(keycode, action, vk_modifiers)

func test_toggle_keyboard() -> void:
    $KeyboardLayer.visible = not $KeyboardLayer.visible

func on_pair_complete(success,message):
    if success:
        print("[Moonlight-Godot-ComputerManager]",message)
    else:
        push_error("[Moonlight-Godot-ComputerManager]",message)

#func _process(delta):
    #var time = $ScrollContainer/GridContainer/AudioStreamPlayer.get_playback_position() + AudioServer.get_time_since_last_mix()
    ## Compensate for output latency.
    #time -= AudioServer.get_output_latency()
    #print("Audio latency Time is: ", time*1000, "ms") if time > 0 else null

func test_http_requeset() -> void:
    
    var url = "http://httpbin.org/uuid"
    var method = "GET"
    var body = PackedByteArray()
    var headers = {}
    var ssl_options = {} # 不附加证书
    var callback = Callable(self, "_on_baidu_request_completed")
    
    requester.request(url, method, body, headers, ssl_options, callback)

@warning_ignore("unused_parameter")
func _on_baidu_request_completed(response_code: int, response_body: PackedByteArray, response_headers: Dictionary, error_text: String) -> void:
    if error_text.is_empty():
        print("请求成功，状态码: ", response_code)
        print("响应内容: ", response_body.get_string_from_utf8())
    else:
        print("请求失败: ", error_text)


func test_cert_create() -> void:
    print(configmanager.get_client_keys())


func test_add_host_info() -> void:
    #var config: Dictionary={
            #"hostname":"TEST",
            #"uuid":Time.get_datetime_string_from_system()
        #}
    #print(configmanager.add_host(config))
    print("current hosts:\n")
    print(configmanager.get_hosts(),"\n")


func test_remove_host_info() -> void:
    configmanager.remove_host(1)


func test_start_pair() -> void:
    var pin = computermamager.start_pair($ScrollContainer/GridContainer/LineEdit.text)
    print("pin:",pin)

func test_cancel_pair() -> void:
    computermamager.unpair(1)


func test_get_applist() -> void:
    computermamager.get_app_list(1)


func test_get_app_texture() -> void:
    configmanager.load_config()
    var applist = configmanager.get_apps(1)
    computermamager.get_app_cover(1,applist[0]["id"],func(texture_return): $ScrollContainer/GridContainer/TextureRect.texture = texture_return)


func test_connect_to_server() -> void:
    computermamager.connect_to_computer($ScrollContainer/GridContainer/LineEdit.text,47989,func(info): print(info))
    


func test_establish_stream() -> void:
    # 使用 Resource API 构建两个配置类并直接启动播放
    var cfg = MoonlightStreamConfigurationResource.new()
    cfg.set_width(1920)
    cfg.set_height(1080)
    cfg.set_fps(60)
    cfg.set_bitrate(int($ScrollContainer/GridContainer/LineEdit2.text) * 10000)
    # 可选：设置 packet_size 或 audio_configuration 等（使用默认则可）
    # cfg.set_packet_size(1392)

    video_enabled = $ScrollContainer/GridContainer/ToggleVideoButton.button_pressed
    audio_enabled = $ScrollContainer/GridContainer/ToggleAudioButton.button_pressed

    var add_opts = MoonlightAdditionalStreamOptions.new()
    add_opts.set_video_codec($ScrollContainer/GridContainer/OptionButton.get_selected_id())
    add_opts.set_disable_hw_acceleration(not $ScrollContainer/GridContainer/CheckButton.button_pressed)
    add_opts.set_disable_video(not video_enabled)
    add_opts.set_disable_audio(not audio_enabled)

    if video_enabled:
        var target = $FullscreenLayer/CenterContainer/FullscreenScreen if is_fullscreen else $ScrollContainer/GridContainer/Screen
        moonlightstreamcore.set_render_target(target)
        
    if audio_enabled:
        $ScrollContainer/GridContainer/AudioStreamPlayer.stream = moonlightstreamcore.get_audio_stream()

    # 直接开始播放（host_id 与 app_id 仍使用示例中的固定值）
    moonlightstreamcore.start_play_stream(1, 1191261554, cfg, add_opts)
    await get_tree().create_timer(1).timeout
    if audio_enabled:
        $ScrollContainer/GridContainer/AudioStreamPlayer.play()

func test_stop_stream() -> void:
    computermamager.stop_stream(1,func(info): print(info);moonlightstreamcore.stop_play_stream();moonlightstreamcore.reset_audio_stream();moonlightstreamcore.reset_render_target())


func test_pause_streram() -> void:
    moonlightstreamcore.stop_play_stream()
    moonlightstreamcore.reset_audio_stream()
    moonlightstreamcore.reset_render_target()


func view_data() -> void:
    $ScrollContainer/FileDialog.visible = true


func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if toggled_on:
        return
    configmanager.update_host(1,{"localaddress":$ScrollContainer/GridContainer/LineEdit.text})


func test_toggle_audio_bypass() -> void:
    if moonlightstreamcore.is_native_audio_bypass_running():
        moonlightstreamcore.stop_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: OFF"
    else:
        var ok = moonlightstreamcore.start_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON" if ok else "Audio Bypass: FAILED"


func test_toggle_audio_pause() -> void:
    if not moonlightstreamcore.is_native_audio_bypass_running():
        push_warning("Native audio bypass not running")
        return
    if moonlightstreamcore.is_native_audio_bypass_paused():
        moonlightstreamcore.resume_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Pause"
    else:
        moonlightstreamcore.pause_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Resume"

func _on_screen_gui_input(event: InputEvent) -> void:
    if not input_enabled:
        return
        
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen if is_fullscreen else $ScrollContainer/GridContainer/Screen
    var screen_width = int(screen.size.x)
    var screen_height = int(screen.size.y)

    if event is InputEventMouseMotion:
        moonlightstreamcore.send_mouse_position_event(int(event.position.x), int(event.position.y), screen_width, screen_height)
    elif event is InputEventMouseButton:
        var action = MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_PRESS if event.pressed else MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_RELEASE
        var button = 0
        match event.button_index:
            MOUSE_BUTTON_LEFT:
                button = MoonlightInput.MouseButton.MOUSE_BUTTON_LEFT
            MOUSE_BUTTON_RIGHT:
                button = MoonlightInput.MouseButton.MOUSE_BUTTON_RIGHT
            MOUSE_BUTTON_MIDDLE:
                button = MoonlightInput.MouseButton.MOUSE_BUTTON_MIDDLE
            MOUSE_BUTTON_WHEEL_UP:
                moonlightstreamcore.send_scroll_event(1)
                return
            MOUSE_BUTTON_WHEEL_DOWN:
                moonlightstreamcore.send_scroll_event(-1)
                return
            MOUSE_BUTTON_XBUTTON1:
                button = MoonlightInput.MouseButton.MOUSE_BUTTON_X1
            MOUSE_BUTTON_XBUTTON2:
                button = MoonlightInput.MouseButton.MOUSE_BUTTON_X2
        if button != 0:
            moonlightstreamcore.send_mouse_button_event(action, button)
    elif event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.shift_pressed:
            modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.ctrl_pressed:
            modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.alt_pressed:
            modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed:
            modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        # Godot keycode to Limelight keycode mapping
        var keycode = event.keycode
        # Basic mapping for common keys, Limelight uses standard USB HID keycodes or similar
        # For simplicity in this test, we pass the Godot keycode directly, 
        # but in a real app, a full mapping table is needed.
        moonlightstreamcore.send_keyboard_event(keycode, action, modifiers)
    elif event is InputEventScreenTouch:
        var action = MoonlightInput.TOUCH_EVENT_DOWN if event.pressed else MoonlightInput.TOUCH_EVENT_UP
        moonlightstreamcore.send_touch_event(action, event.index, event.position.x / screen_width, event.position.y / screen_height, 1.0, 0.0, 0.0, 0)
    elif event is InputEventScreenDrag:
        moonlightstreamcore.send_touch_event(MoonlightInput.TOUCH_EVENT_MOVE, event.index, event.position.x / screen_width, event.position.y / screen_height, 1.0, 0.0, 0.0, 0)

func _on_fullscreen_pressed() -> void:
    is_fullscreen = true
    $FullscreenLayer.visible = true
    if video_enabled:
        moonlightstreamcore.set_render_target($FullscreenLayer/CenterContainer/FullscreenScreen)

func _on_exit_fullscreen_pressed() -> void:
    is_fullscreen = false
    $FullscreenLayer.visible = false
    if video_enabled:
        moonlightstreamcore.set_render_target($ScrollContainer/GridContainer/Screen)

func _on_toggle_input_pressed() -> void:
    input_enabled = $ScrollContainer/GridContainer/ToggleInputButton.button_pressed
