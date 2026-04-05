extends Node

# -----------------
# Moonlight 核心组件
# -----------------
@onready var moonlight_requester = MoonlightRequester.new()
@onready var moonlight_config_manager = MoonlightConfigManager.new()
@onready var moonlight_computer_manager = MoonlightComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

# -----------------
# 串流状态及控制变量
# -----------------
var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var is_fullscreen: bool = false
var current_stream_config: MoonlightStreamConfigurationResource = null
var vk_modifiers: int = 0  # 虚拟键盘的修饰键状态掩码

func _ready() -> void:
    # 依赖注入：为管理器和核心设置配置管理器
    moonlight_computer_manager.set_config_manager(moonlight_config_manager)
    moonlightstreamcore.set_config_manager(moonlight_config_manager)
    
    # 信号连接：监听配对结果和串流状态
    moonlight_computer_manager.pair_completed.connect(on_pair_complete)
    moonlightstreamcore.connection_started.connect(func(): print("[Moonlight-Godot-MoonlightStreamCore]", "Connect Successfully!"))
    moonlightstreamcore.connection_terminated.connect(func(_err, msg): push_error("[Moonlight-Godot-MoonlightStreamCore]", msg))
    
    @warning_ignore("standalone_ternary")
    moonlightstreamcore.hdr_mode_changed.connect(func(enable, data): print("[Moonlight-Godot-MoonlightStreamCore-HDR]", data) if enable else push_warning("[Moonlight-Godot-MoonlightStreamCore-HDR]", data))
    
    # 初始化 UI 显示的主机 IP（如果存在历史记录）
    if moonlight_config_manager.get_hosts().size() > 0:
        $ScrollContainer/GridContainer/LineEdit.text = moonlight_config_manager.get_hosts()[0].localaddress
    
    # 初始化音频直通按钮文本状态
    if $ScrollContainer/GridContainer.has_node("AudioBypassButton"):
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON" if moonlightstreamcore.is_native_audio_bypass_running() else "Audio Bypass: OFF"
    if $ScrollContainer/GridContainer.has_node("AudioPauseButton"):
        var paused = moonlightstreamcore.is_native_audio_bypass_paused()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Resume" if paused else "Pause"
    
    # 绑定控制按钮事件
    if $ScrollContainer/GridContainer.has_node("FullscreenButton"):
        $ScrollContainer/GridContainer/FullscreenButton.pressed.connect(_on_fullscreen_pressed)
        $ScrollContainer/GridContainer/ToggleInputButton.pressed.connect(_on_toggle_input_pressed)
        $FullscreenLayer/ExitFullscreenButton.pressed.connect(_on_exit_fullscreen_pressed)
        $FullscreenLayer/CenterContainer/FullscreenScreen.gui_input.connect(_on_screen_gui_input)
    
    # 初始化屏幕虚拟键盘
    _setup_virtual_keyboard()

func _setup_virtual_keyboard() -> void:
    var row0 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row0
    var row1 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row1
    var row2 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row2
    var row3 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row3
    var row4 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row4
    var row5 = $KeyboardLayer/KeyboardPanel/VBoxContainer/Row5
    
    var keys_r0 = [
        {"text": "Esc", "code": MoonlightInput.VK_ESCAPE}, {"blank": true, "w": 25},
        {"text": "F1", "code": MoonlightInput.VK_F1}, {"text": "F2", "code": MoonlightInput.VK_F2}, {"text": "F3", "code": MoonlightInput.VK_F3}, {"text": "F4", "code": MoonlightInput.VK_F4}, {"blank": true, "w": 25},
        {"text": "F5", "code": MoonlightInput.VK_F5}, {"text": "F6", "code": MoonlightInput.VK_F6}, {"text": "F7", "code": MoonlightInput.VK_F7}, {"text": "F8", "code": MoonlightInput.VK_F8}, {"blank": true, "w": 25},
        {"text": "F9", "code": MoonlightInput.VK_F9}, {"text": "F10", "code": MoonlightInput.VK_F10}, {"text": "F11", "code": MoonlightInput.VK_F11}, {"text": "F12", "code": MoonlightInput.VK_F12}, {"blank": true, "w": 25},
        {"text": "PrtSc", "code": MoonlightInput.VK_PRINT}, {"text": "ScrLk", "code": MoonlightInput.VK_SCROLL}, {"text": "Pause", "code": MoonlightInput.VK_PAUSE}, {"blank": true, "w": 25},
        {"text": "Hide", "code": -1}
    ]
    var keys_r1 = [
        {"text": "`", "code": MoonlightInput.VK_OEM_3}, {"text": "1", "code": MoonlightInput.VK_1}, {"text": "2", "code": MoonlightInput.VK_2}, {"text": "3", "code": MoonlightInput.VK_3}, {"text": "4", "code": MoonlightInput.VK_4}, {"text": "5", "code": MoonlightInput.VK_5}, {"text": "6", "code": MoonlightInput.VK_6}, {"text": "7", "code": MoonlightInput.VK_7}, {"text": "8", "code": MoonlightInput.VK_8}, {"text": "9", "code": MoonlightInput.VK_9}, {"text": "0", "code": MoonlightInput.VK_0}, {"text": "-", "code": MoonlightInput.VK_OEM_MINUS}, {"text": "=", "code": MoonlightInput.VK_OEM_PLUS}, {"text": "Backspace", "code": MoonlightInput.VK_BACK, "w": 100},
        {"blank": true, "w": 25},
        {"text": "Ins", "code": MoonlightInput.VK_INSERT}, {"text": "Home", "code": MoonlightInput.VK_HOME}, {"text": "PgUp", "code": MoonlightInput.VK_PRIOR},
        {"blank": true, "w": 25},
        {"text": "NumLk", "code": MoonlightInput.VK_NUMLOCK}, {"text": "/", "code": MoonlightInput.VK_DIVIDE}, {"text": "*", "code": MoonlightInput.VK_MULTIPLY}, {"text": "-", "code": MoonlightInput.VK_SUBTRACT}
    ]
    var keys_r2 = [
        {"text": "Tab", "code": MoonlightInput.VK_TAB, "w": 75}, {"text": "Q", "code": MoonlightInput.VK_Q}, {"text": "W", "code": MoonlightInput.VK_W}, {"text": "E", "code": MoonlightInput.VK_E}, {"text": "R", "code": MoonlightInput.VK_R}, {"text": "T", "code": MoonlightInput.VK_T}, {"text": "Y", "code": MoonlightInput.VK_Y}, {"text": "U", "code": MoonlightInput.VK_U}, {"text": "I", "code": MoonlightInput.VK_I}, {"text": "O", "code": MoonlightInput.VK_O}, {"text": "P", "code": MoonlightInput.VK_P}, {"text": "[", "code": MoonlightInput.VK_OEM_4}, {"text": "]", "code": MoonlightInput.VK_OEM_6}, {"text": "\\", "code": MoonlightInput.VK_OEM_5, "w": 75},
        {"blank": true, "w": 25},
        {"text": "Del", "code": MoonlightInput.VK_DELETE}, {"text": "End", "code": MoonlightInput.VK_END}, {"text": "PgDn", "code": MoonlightInput.VK_NEXT},
        {"blank": true, "w": 25},
        {"text": "7", "code": MoonlightInput.VK_NUMPAD7}, {"text": "8", "code": MoonlightInput.VK_NUMPAD8}, {"text": "9", "code": MoonlightInput.VK_NUMPAD9}, {"text": "+", "code": MoonlightInput.VK_ADD}
    ]
    var keys_r3 = [
        {"text": "Caps", "code": MoonlightInput.VK_CAPITAL, "w": 90}, {"text": "A", "code": MoonlightInput.VK_A}, {"text": "S", "code": MoonlightInput.VK_S}, {"text": "D", "code": MoonlightInput.VK_D}, {"text": "F", "code": MoonlightInput.VK_F}, {"text": "G", "code": MoonlightInput.VK_G}, {"text": "H", "code": MoonlightInput.VK_H}, {"text": "J", "code": MoonlightInput.VK_J}, {"text": "K", "code": MoonlightInput.VK_K}, {"text": "L", "code": MoonlightInput.VK_L}, {"text": ";", "code": MoonlightInput.VK_OEM_1}, {"text": "'", "code": MoonlightInput.VK_OEM_7}, {"text": "Enter", "code": MoonlightInput.VK_RETURN, "w": 114},
        {"blank": true, "w": 25},
        {"blank": true, "w": 158},
        {"blank": true, "w": 25},
        {"text": "4", "code": MoonlightInput.VK_NUMPAD4}, {"text": "5", "code": MoonlightInput.VK_NUMPAD5}, {"text": "6", "code": MoonlightInput.VK_NUMPAD6}, {"blank": true, "w": 50}
    ]
    var keys_r4 = [
        {"text": "Shift", "code": MoonlightInput.VK_LSHIFT, "toggle": true, "w": 115}, {"text": "Z", "code": MoonlightInput.VK_Z}, {"text": "X", "code": MoonlightInput.VK_X}, {"text": "C", "code": MoonlightInput.VK_C}, {"text": "V", "code": MoonlightInput.VK_V}, {"text": "B", "code": MoonlightInput.VK_B}, {"text": "N", "code": MoonlightInput.VK_N}, {"text": "M", "code": MoonlightInput.VK_M}, {"text": ",", "code": MoonlightInput.VK_OEM_COMMA}, {"text": ".", "code": MoonlightInput.VK_OEM_PERIOD}, {"text": "/", "code": MoonlightInput.VK_OEM_2}, {"text": "Shift", "code": MoonlightInput.VK_RSHIFT, "toggle": true, "w": 143},
        {"blank": true, "w": 25},
        {"blank": true, "w": 50}, {"text": "↑", "code": MoonlightInput.VK_UP}, {"blank": true, "w": 50},
        {"blank": true, "w": 25},
        {"text": "1", "code": MoonlightInput.VK_NUMPAD1}, {"text": "2", "code": MoonlightInput.VK_NUMPAD2}, {"text": "3", "code": MoonlightInput.VK_NUMPAD3}, {"text": "Ent", "code": MoonlightInput.VK_RETURN}
    ]
    var keys_r5 = [
        {"text": "Ctrl", "code": MoonlightInput.VK_LCONTROL, "toggle": true, "w": 60}, {"text": "Win", "code": MoonlightInput.VK_LWIN, "toggle": true, "w": 60}, {"text": "Alt", "code": MoonlightInput.VK_LMENU, "toggle": true, "w": 60}, {"text": "Space", "code": MoonlightInput.VK_SPACE, "w": 354}, {"text": "Alt", "code": MoonlightInput.VK_RMENU, "toggle": true, "w": 60}, {"text": "Win", "code": MoonlightInput.VK_RWIN, "toggle": true, "w": 60}, {"text": "Menu", "code": MoonlightInput.VK_APPS, "w": 60}, {"text": "Ctrl", "code": MoonlightInput.VK_RCONTROL, "toggle": true, "w": 60},
        {"blank": true, "w": 25},
        {"text": "←", "code": MoonlightInput.VK_LEFT}, {"text": "↓", "code": MoonlightInput.VK_DOWN}, {"text": "→", "code": MoonlightInput.VK_RIGHT},
        {"blank": true, "w": 25},
        {"text": "0", "code": MoonlightInput.VK_NUMPAD0, "w": 104}, {"text": ".", "code": MoonlightInput.VK_DECIMAL}, {"blank": true, "w": 50}
    ]
    
    var create_btn = func(k: Dictionary, parent: Node):
        if k.get("blank", false):
            var spacer = Control.new()
            spacer.custom_minimum_size = Vector2(k.get("w", 50), 50)
            parent.add_child(spacer)
            return
            
        var btn = Button.new()
        btn.text = k["text"]
        btn.custom_minimum_size = Vector2(k.get("w", 50), 50)
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
        
    for k in keys_r0: create_btn.call(k, row0)
    for k in keys_r1: create_btn.call(k, row1)
    for k in keys_r2: create_btn.call(k, row2)
    for k in keys_r3: create_btn.call(k, row3)
    for k in keys_r4: create_btn.call(k, row4)
    for k in keys_r5: create_btn.call(k, row5)

func _on_vk_modifier_toggled(keycode: int, pressed: bool) -> void:
    var mod_bit = 0
    if keycode == MoonlightInput.VK_LCONTROL or keycode == MoonlightInput.VK_RCONTROL: mod_bit = MoonlightInput.MODIFIER_CTRL_BIT
    elif keycode == MoonlightInput.VK_LSHIFT or keycode == MoonlightInput.VK_RSHIFT: mod_bit = MoonlightInput.MODIFIER_SHIFT_BIT
    elif keycode == MoonlightInput.VK_LMENU or keycode == MoonlightInput.VK_RMENU: mod_bit = MoonlightInput.MODIFIER_ALT_BIT
    elif keycode == MoonlightInput.VK_LWIN or keycode == MoonlightInput.VK_RWIN: mod_bit = MoonlightInput.MODIFIER_META_BIT
    
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
        print("[Moonlight-Godot-MoonlightComputerManager]",message)
    else:
        push_error("[Moonlight-Godot-MoonlightComputerManager]",message)

#func _process(delta):
    #var time = $ScrollContainer/GridContainer/AudioStreamPlayer.get_playback_position() + AudioServer.get_time_since_last_mix()
    ## Compensate for output latency.
    #time -= AudioServer.get_output_latency()
    #print("Audio latency Time is: ", time*1000, "ms") if time > 0 else null

func test_http_requeset() -> void:
    # HTTP 请求测试：使用 MoonlightRequester，进行基础的 GET 请求
    var url = "http://httpbin.org/uuid"
    var method = "GET"
    var body = PackedByteArray()
    var headers = {}
    var ssl_options = {} # { "certificate": "..." } 等可以由此配置
    var callback = Callable(self, "_on_baidu_request_completed")
    
    moonlight_requester.request(url, method, body, headers, ssl_options, callback)

@warning_ignore("unused_parameter")
func _on_baidu_request_completed(response_code: int, response_body: PackedByteArray, response_headers: Dictionary, error_text: String) -> void:
    if error_text.is_empty():
        print("请求成功，状态码: ", response_code)
        print("响应内容: ", response_body.get_string_from_utf8())
    else:
        print("请求失败: ", error_text)

func test_cert_create() -> void:
    # 打印通过 ConfigManager 从磁盘/系统读取的客户端证书和配对密钥信息
    print(moonlight_config_manager.get_client_keys())

func test_add_host_info() -> void:
    # 获取并打印被保存在本地配置文件中的所有已添加服务器（Hosts）信息
    print("current hosts:\n")
    print(moonlight_config_manager.get_hosts(),"\n")

func test_remove_host_info() -> void:
    # 从配置文件中移除 ID 为 1 的主机的连接/配对信息
    moonlight_config_manager.remove_host(1)

func test_start_pair() -> void:
    # 与指定 IP 地址的机器发起配对流程，打印出的 PIN 码需要在被串流机器上的 NVIDIA 弹窗中填入
    var pin = moonlight_computer_manager.start_pair($ScrollContainer/GridContainer/LineEdit.text)
    print("pin:",pin)

func test_cancel_pair() -> void:
    # 请求对应 ID 的主机取消先前的配对凭证信息
    moonlight_computer_manager.unpair(1)

func test_get_applist() -> void:
    # 通过设备 ID 获取其可用 App（通常是串流游戏列表）的元数据列
    moonlight_computer_manager.get_app_list(1)


func test_get_app_texture() -> void:
    # 重新加载本地配置和 App 列表缓存，获取并渲染目标游封面的 Texture 对象到 UI
    moonlight_config_manager.load_config()
    var applist = moonlight_config_manager.get_apps(1)
    if applist.size() > 0:
        moonlight_computer_manager.get_app_cover(1, applist[0]["id"], func(texture_return): $ScrollContainer/GridContainer/TextureRect.texture = texture_return)

func test_connect_to_server() -> void:
    # 尝试测试特定主机的连接通道（主要用于连接握手测试，非串流数据通道）
    moonlight_computer_manager.connect_to_computer($ScrollContainer/GridContainer/LineEdit.text, 47989, func(info): print(info))
    


# -----------------
# 串流生命周期管理 (重点参考部分)
# -----------------

func test_establish_stream() -> void:
    # 步骤1. 使用 Resource API 构建串流配置参数 (MoonlightStreamConfigurationResource)
    var cfg = MoonlightStreamConfigurationResource.new()
    cfg.set_width(1920)
    cfg.set_height(1080)
    cfg.set_fps(60)
    
    # 获取比特率设定，转换为 bps (10000 可能是由于UI上输入的是 Mbps)
    # 此处严谨验证一下文本框数字
    var bitrate_text = $ScrollContainer/GridContainer/LineEdit2.text
    if bitrate_text.is_valid_float():
        cfg.set_bitrate(int(float(bitrate_text) * 10000))
    else:
        cfg.set_bitrate(20000 * 1000) # 默认 20 Mbps
        
    # 可选：设置 packet_size 或 audio_configuration 等（使用默认也可）
    # cfg.set_packet_size(1392)
    
    current_stream_config = cfg

    # 获取当前 UI 面板的视音频状态
    video_enabled = $ScrollContainer/GridContainer/ToggleVideoButton.button_pressed
    audio_enabled = $ScrollContainer/GridContainer/ToggleAudioButton.button_pressed

    # 步骤2. 配置附加参数对象 (MoonlightAdditionalStreamOptions)
    var add_opts = MoonlightAdditionalStreamOptions.new()
    add_opts.set_video_codec($ScrollContainer/GridContainer/OptionButton.get_selected_id())
    add_opts.set_disable_hw_acceleration(not $ScrollContainer/GridContainer/CheckButton.button_pressed)
    add_opts.set_disable_video(not video_enabled)
    add_opts.set_disable_audio(not audio_enabled)

    # 步骤3. 为视频流指定渲染目标（如启用的话）
    if video_enabled:
        var target = $FullscreenLayer/CenterContainer/FullscreenScreen if is_fullscreen else $ScrollContainer/GridContainer/Screen
        moonlightstreamcore.set_render_target(target)
        
    # 步骤4. 为音频流指派对应的播放节点（提前准备好 AudioStream 的绑定）
    if audio_enabled:
        # get_audio_stream() 将返回核心接收音频后生成的 AudioStream 对象
        $ScrollContainer/GridContainer/AudioStreamPlayer.stream = moonlightstreamcore.get_audio_stream()

    # 步骤5. 启动串流
    # 注意：这里的 host_id (1) 与 app_id (1191261554) 仍使用示例中的固定值，一般需要通过 get_app_list 动态获取
    moonlightstreamcore.start_play_stream(1, 1191261554, cfg, add_opts)
    
    # 步骤6. 延迟短时间后启动音频节点播放 (等待流缓存稳定)
    await get_tree().create_timer(1).timeout
    if audio_enabled:
        # 下面被注释的 streams 列表常用于多流播放，单流场景用 get_audio_stream 即可
        # var streams = moonlightstreamcore.get_audio_streams()
        # $ScrollContainer/GridContainer/AudioStreamPlayer.stream = streams[0]
        $ScrollContainer/GridContainer/AudioStreamPlayer.play()

func test_stop_stream() -> void:
    # 请求计算机管理器断开连接
    moonlight_computer_manager.stop_stream(1, func(info): 
        print("[Moonlight-Godot] Stop Stream:", info)
        
        # 停止核心接收循环和清空绑定
        moonlightstreamcore.stop_play_stream()
        moonlightstreamcore.reset_audio_stream()
        moonlightstreamcore.reset_render_target()
    )

func test_pause_streram() -> void:
    # 模拟暂停操作（单纯从 Godot 后端断开本地音视频渲染，不断开远端连接）
    moonlightstreamcore.stop_play_stream()
    moonlightstreamcore.reset_audio_stream()
    moonlightstreamcore.reset_render_target()


func view_data() -> void:
    $ScrollContainer/FileDialog.visible = true

func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if toggled_on:
        return
    # 编辑完成后，同步更改到主机的本地缓存配置文件
    moonlight_config_manager.update_host(1, {"localaddress": $ScrollContainer/GridContainer/LineEdit.text})

func test_toggle_audio_bypass() -> void:
    # 切换系统原生音频旁路（将音频流直接推给系统驱动，减少延迟，不再经由 Godot 滤镜层处理）
    if moonlightstreamcore.is_native_audio_bypass_running():
        moonlightstreamcore.stop_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: OFF"
    else:
        var ok = moonlightstreamcore.start_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON" if ok else "Audio Bypass: FAILED"

func test_toggle_audio_pause() -> void:
    # 在原生音频旁路运行期间对其进行暂停和恢复操作
    if not moonlightstreamcore.is_native_audio_bypass_running():
        push_warning("Native audio bypass not running")
        return
    if moonlightstreamcore.is_native_audio_bypass_paused():
        moonlightstreamcore.resume_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Pause"
    else:
        moonlightstreamcore.pause_native_audio_bypass()
        $ScrollContainer/GridContainer/AudioPauseButton.text = "Audio Bypass Resume"

# -----------------
# 用户输入与交互反馈
# -----------------

func _on_screen_gui_input(event: InputEvent) -> void:
    if not input_enabled:
        return
        
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen if is_fullscreen else $ScrollContainer/GridContainer/Screen
    var screen_width = int(screen.size.x)
    var screen_height = int(screen.size.y)

    # 1. 鼠标移动/光标偏移
    if event is InputEventMouseMotion:
        moonlightstreamcore.send_mouse_position_event(int(event.position.x), int(event.position.y), screen_width, screen_height)
        
    # 2. 鼠标点击/滚轮行为
    elif event is InputEventMouseButton:
        var action = MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_PRESS if event.pressed else MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_RELEASE
        var button = 0
        match event.button_index:
            MOUSE_BUTTON_LEFT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_LEFT
            MOUSE_BUTTON_RIGHT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_RIGHT
            MOUSE_BUTTON_MIDDLE: button = MoonlightInput.MouseButton.MOUSE_BUTTON_MIDDLE
            MOUSE_BUTTON_WHEEL_UP:
                moonlightstreamcore.send_scroll_event(1)
                return
            MOUSE_BUTTON_WHEEL_DOWN:
                moonlightstreamcore.send_scroll_event(-1)
                return
            MOUSE_BUTTON_XBUTTON1: button = MoonlightInput.MouseButton.MOUSE_BUTTON_X1
            MOUSE_BUTTON_XBUTTON2: button = MoonlightInput.MouseButton.MOUSE_BUTTON_X2
            
        if button != 0:
            moonlightstreamcore.send_mouse_button_event(action, button)
            
    # 3. 实体键盘发送处理
    elif event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.shift_pressed: modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.ctrl_pressed: modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.alt_pressed: modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed: modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        # MoonlightCore 模块后台已经封装了从 Godot keycode 到对应 HID Keycode 的映射处理，可直接传递。
        var keycode = event.keycode
        moonlightstreamcore.send_keyboard_event(keycode, action, modifiers)
        
    # 4. 移动设备屏幕触控适配（包括点击和拖动）
    elif event is InputEventScreenTouch:
        var action = MoonlightInput.TOUCH_EVENT_DOWN if event.pressed else MoonlightInput.TOUCH_EVENT_UP
        moonlightstreamcore.send_touch_event(action, event.index, event.position.x / screen_width, event.position.y / screen_height, 1.0, 0.0, 0.0, 0)
    elif event is InputEventScreenDrag:
        moonlightstreamcore.send_touch_event(MoonlightInput.TOUCH_EVENT_MOVE, event.index, event.position.x / screen_width, event.position.y / screen_height, 1.0, 0.0, 0.0, 0)

func _on_fullscreen_pressed() -> void:
    is_fullscreen = true
    $FullscreenLayer.visible = true
    
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen
    
    # 无论是否禁用视频，都手动设置 TextureRect 的最小尺寸以匹配串流参数比例
    # 这样可以确保画面或输入区域始终保持正确的比例并缩放到全屏
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
        # 窗口更宽，以高度为基准
        target_size.y = window_size.y
        target_size.x = target_size.y * stream_ratio
    else:
        # 窗口更高，以宽度为基准
        target_size.x = window_size.x
        target_size.y = target_size.x / stream_ratio
        
    screen.custom_minimum_size = target_size

    if video_enabled:
        moonlightstreamcore.set_render_target(screen)

func _on_exit_fullscreen_pressed() -> void:
    is_fullscreen = false
    $FullscreenLayer.visible = false
    
    var screen = $FullscreenLayer/CenterContainer/FullscreenScreen
    screen.custom_minimum_size = Vector2(0, 0) # 恢复默认
    
    if video_enabled:
        moonlightstreamcore.set_render_target($ScrollContainer/GridContainer/Screen)

func _on_toggle_input_pressed() -> void:
    input_enabled = $ScrollContainer/GridContainer/ToggleInputButton.button_pressed


func _on_check_button_2_toggled(toggled_on: bool) -> void:
    $ScrollContainer/GridContainer/AudioStreamPlayer.playing = toggled_on
