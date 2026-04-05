extends Node3D

# -----------------
# Moonlight 核心组件
# -----------------
@onready var moonlight_requester = MoonlightRequester.new()
@onready var moonlight_config_manager = MoonlightConfigManager.new()
@onready var moonlight_computer_manager = MoonlightComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

# -----------------
# UI 相关的节点引用
# -----------------
@onready var ui_layer = $UILayer
@onready var settings_panel = $UILayer/SettingsPanel
@onready var settings_btn = $UILayer/SettingsButton
@onready var close_btn = $UILayer/SettingsPanel/VBoxContainer/HBoxContainer/CloseButton
@onready var scroll_container = $UILayer/SettingsPanel/VBoxContainer/ScrollContainer
@onready var grid_container = $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer
@onready var line_edit_ip = grid_container.get_node("LineEdit")
@onready var line_edit_bitrate = grid_container.get_node("LineEdit2")
@onready var texture_rect_app = grid_container.get_node("TextureRect")

# -----------------
# 3D 环境与物理节点引用
# -----------------
@onready var player = $Player
@onready var head = $Player/Head
@onready var screen_mesh = $TVModel/ScreenMesh
@onready var stream_viewport = $StreamViewport
@onready var stream_texture = $StreamViewport/StreamTexture
@onready var audio_left = $TVModel/AudioLeft
@onready var audio_right = $TVModel/AudioRight
@onready var camera = $Player/Head/Camera3D

# 要输出空间音频的 AudioStreamPlayer3D 组件列表
@export var audio_players: Array[AudioStreamPlayer3D]

# -----------------
# 串流与功能状态
# -----------------
var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var godot_audio_enabled: bool = true # 控制是否通过 Godot 节点播放回声 (空间音频需要此项设定为 true)

# -----------------
# 第三人称漫游运动参数
# -----------------
const SPEED = 5.0
const JUMP_VELOCITY = 4.5
const MOUSE_SENSITIVITY = 0.003
const JOYSTICK_LOOK_SPEED = 2.6

# 交互模式枚举：步行模式（漫游）或串流控制模式（焦点被捕获并传输给云端主机）
enum PlayMode {
    WALK,
    STREAM_CONTROL
}
var current_mode = PlayMode.WALK

# 移动端虚拟摇杆控件声明
var is_mobile_platform: bool = false
var mobile_controls: Control
var joystick_base: ColorRect
var joystick_knob: ColorRect
var joystick_radius: float = 65.0
var joystick_touch_id: int = -1
var joystick_vector: Vector2 = Vector2.ZERO
var move_joystick_base: ColorRect
var move_joystick_knob: ColorRect
var move_joystick_radius: float = 65.0
var move_joystick_touch_id: int = -1
var move_joystick_vector: Vector2 = Vector2.ZERO
var jump_btn: Button
var mobile_jump_requested: bool = false

func _ready() -> void:
    # --- 材质应用配置(重点) ---
    # 1. 确保目标显示器的网格使用了唯一材质
    var mat = screen_mesh.get_active_material(0)
    if not mat or not (mat is StandardMaterial3D):
        mat = StandardMaterial3D.new()
        screen_mesh.material_override = mat

    # 2. 将此材质变更为无光照模式(Unshaded，用于自发光模拟屏幕)，并配置双面剔除机制
    mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    mat.albedo_color = Color.WHITE
    mat.cull_mode = BaseMaterial3D.CULL_DISABLED 
    
    # 3. 将用于接收视频管线数据的 Viewport 贴图直接指定到模型表面材质中
    mat.albedo_texture = stream_viewport.get_texture()
    print("Assigned Viewport Texture to Screen Material: ", mat.albedo_texture)

    # 4. Viewport 中内置拉伸模式初始化
    stream_texture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
    stream_texture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED

    # --- Godot 内置空间音频配置 ---
    # 对每一个挂载注册的三维音频源（通常布置于电视两端）进行距离衰减设置
    for ap in audio_players:
        if ap and ap is AudioStreamPlayer3D:
            ap.unit_size = 20.0
            ap.max_distance = 50.0
            ap.attenuation_model = AudioStreamPlayer3D.ATTENUATION_INVERSE_DISTANCE
        else:
            print("Warning: audio_players contains non-AudioStreamPlayer3D or null:", ap)
    
    # --------------------------------
    # 初始化 UI 界面及鼠标捕获
    settings_panel.visible = false
    Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
    
    # 连接 Moonlight 各生命周期回调和串流日志捕获信号
    moonlight_computer_manager.pair_completed.connect(on_pair_complete)
    moonlightstreamcore.connection_started.connect(func(): print("[Moonlight-Godot-MoonlightStreamCore]", "Connect Successfully!"))
    moonlightstreamcore.connection_terminated.connect(func(_err, msg): push_error("[Moonlight-Godot-MoonlightStreamCore]", msg))
    
    # 从配置文件载入历史保存的主机
    if moonlight_config_manager.get_hosts().size() > 0:
        line_edit_ip.text = moonlight_config_manager.get_hosts()[0].localaddress
    else:
        line_edit_ip.text = "192.168.1.1"

    # UI 回调挂载
    settings_btn.pressed.connect(toggle_settings)
    close_btn.pressed.connect(toggle_settings)
    
    # UI 开关（手动连接状态切换）
    connect_button_signal("ToggleVideoButton", "toggled", func(t): video_enabled = t)
    connect_button_signal("ToggleAudioButton", "toggled", func(t): audio_enabled = t)
    connect_button_signal("ToggleInputButton", "toggled", func(t): input_enabled = t)
    
    # 连接杂项控制及旁路音频功能开关
    connect_button_signal("FullscreenButton", "pressed", toggle_fullscreen)
    connect_button_signal("CheckButton2", "toggled", Callable(self, "set_godot_audio_enabled")) 
    connect_button_signal("AudioBypassButton", "pressed", toggle_audio_bypass)
    connect_button_signal("AudioPauseButton", "pressed", toggle_pause_audio_bypass)
    connect_button_signal("KeyboardButton", "pressed", toggle_virtual_keyboard)

    # 初始刷新Godot空间音频状态
    set_godot_audio_enabled(godot_audio_enabled)

    # 移动平台摇杆模拟按键初始化
    is_mobile_platform = OS.has_feature("mobile")
    if is_mobile_platform:
        setup_mobile_controls()

func setup_mobile_controls() -> void:
    mobile_controls = Control.new()
    mobile_controls.name = "MobileControls"
    mobile_controls.set_anchors_preset(Control.PRESET_FULL_RECT)
    mobile_controls.mouse_filter = Control.MOUSE_FILTER_IGNORE
    ui_layer.add_child(mobile_controls)

    joystick_base = ColorRect.new()
    joystick_base.name = "LookJoystickBase"
    joystick_base.color = Color(0.1, 0.1, 0.1, 0.35)
    joystick_base.custom_minimum_size = Vector2(150, 150)
    joystick_base.position = Vector2(0, 0)
    joystick_base.mouse_filter = Control.MOUSE_FILTER_IGNORE
    mobile_controls.add_child(joystick_base)

    joystick_knob = ColorRect.new()
    joystick_knob.name = "LookJoystickKnob"
    joystick_knob.color = Color(0.9, 0.9, 0.9, 0.6)
    joystick_knob.custom_minimum_size = Vector2(62, 62)
    joystick_knob.mouse_filter = Control.MOUSE_FILTER_IGNORE
    joystick_base.add_child(joystick_knob)

    move_joystick_base = ColorRect.new()
    move_joystick_base.name = "MoveJoystickBase"
    move_joystick_base.color = Color(0.1, 0.1, 0.1, 0.35)
    move_joystick_base.custom_minimum_size = Vector2(150, 150)
    move_joystick_base.mouse_filter = Control.MOUSE_FILTER_IGNORE
    mobile_controls.add_child(move_joystick_base)

    move_joystick_knob = ColorRect.new()
    move_joystick_knob.name = "MoveJoystickKnob"
    move_joystick_knob.color = Color(0.9, 0.9, 0.9, 0.6)
    move_joystick_knob.custom_minimum_size = Vector2(62, 62)
    move_joystick_knob.mouse_filter = Control.MOUSE_FILTER_IGNORE
    move_joystick_base.add_child(move_joystick_knob)

    jump_btn = Button.new()
    jump_btn.name = "JumpButton"
    jump_btn.text = "跳跃"
    jump_btn.custom_minimum_size = Vector2(110, 54)
    jump_btn.button_down.connect(on_mobile_jump_pressed)
    mobile_controls.add_child(jump_btn)

    update_mobile_controls_layout()

func update_mobile_controls_layout() -> void:
    if not mobile_controls or not joystick_base or not joystick_knob or not move_joystick_base or not move_joystick_knob:
        return

    var vp_size = get_viewport().get_visible_rect().size
    move_joystick_base.position = Vector2(40.0, vp_size.y - 190.0)
    joystick_base.position = Vector2(vp_size.x - 190.0, vp_size.y - 190.0)
    joystick_knob.position = (joystick_base.size - joystick_knob.size) * 0.5 + joystick_vector * joystick_radius
    move_joystick_knob.position = (move_joystick_base.size - move_joystick_knob.size) * 0.5 + move_joystick_vector * move_joystick_radius
    if jump_btn:
        jump_btn.position = Vector2(vp_size.x - 140.0, vp_size.y - 300.0)

func reset_joystick() -> void:
    joystick_touch_id = -1
    joystick_vector = Vector2.ZERO
    if joystick_base and joystick_knob:
        joystick_knob.position = (joystick_base.size - joystick_knob.size) * 0.5

func reset_move_joystick() -> void:
    move_joystick_touch_id = -1
    move_joystick_vector = Vector2.ZERO
    if move_joystick_base and move_joystick_knob:
        move_joystick_knob.position = (move_joystick_base.size - move_joystick_knob.size) * 0.5

func process_mobile_joystick_event(event: InputEvent) -> void:
    if not is_mobile_platform or not joystick_base or not move_joystick_base:
        return

    if event is InputEventScreenTouch:
        if event.pressed:
            if move_joystick_touch_id == -1 and move_joystick_base.get_global_rect().has_point(event.position):
                move_joystick_touch_id = event.index
                update_move_joystick_from_position(event.position)
            elif joystick_touch_id == -1 and joystick_base.get_global_rect().has_point(event.position):
                joystick_touch_id = event.index
                update_joystick_from_position(event.position)
        else:
            if event.index == joystick_touch_id:
                reset_joystick()
            elif event.index == move_joystick_touch_id:
                reset_move_joystick()

    elif event is InputEventScreenDrag:
        if event.index == joystick_touch_id:
            update_joystick_from_position(event.position)
        elif event.index == move_joystick_touch_id:
            update_move_joystick_from_position(event.position)

func update_joystick_from_position(screen_pos: Vector2) -> void:
    var base_center = joystick_base.get_global_rect().position + joystick_base.size * 0.5
    var delta = screen_pos - base_center
    if delta.length() > joystick_radius:
        delta = delta.normalized() * joystick_radius

    joystick_vector = delta / joystick_radius
    joystick_knob.position = (joystick_base.size - joystick_knob.size) * 0.5 + delta

func update_move_joystick_from_position(screen_pos: Vector2) -> void:
    var base_center = move_joystick_base.get_global_rect().position + move_joystick_base.size * 0.5
    var delta = screen_pos - base_center
    if delta.length() > move_joystick_radius:
        delta = delta.normalized() * move_joystick_radius

    move_joystick_vector = delta / move_joystick_radius
    move_joystick_knob.position = (move_joystick_base.size - move_joystick_knob.size) * 0.5 + delta

func on_mobile_jump_pressed() -> void:
    mobile_jump_requested = true

func connect_button_signal(node_name: String, signal_name: String, callable: Callable):
    if grid_container.has_node(node_name):
        var node = grid_container.get_node(node_name)
        if not node.is_connected(signal_name, callable):
            node.connect(signal_name, callable)
    else:
        print("Note: Button not found: ", node_name)

func _input(event: InputEvent) -> void:
    if is_mobile_platform:
        process_mobile_joystick_event(event)

    # 1. Global Toggle for Settings (ESC)
    if event.is_action_pressed("ui_cancel"): # ESC
        toggle_settings()
        get_viewport().set_input_as_handled()
        return
    
    # 2. If Settings Panel is Open:
    if settings_panel.visible:
        # Block game input, allow UI interaction only.
        return

    # 3. If Settings Panel is Hidden:
    
    # Toggle Mode with TAB
    if event.is_action_pressed("ui_focus_next"): # TAB
        toggle_mode()
        get_viewport().set_input_as_handled()
        return

    # Handle Mode Specific Input
    if current_mode == PlayMode.WALK:
        handle_walk_input(event)
    elif current_mode == PlayMode.STREAM_CONTROL:
        handle_stream_input(event)

func toggle_settings():
    settings_panel.visible = !settings_panel.visible
    if settings_panel.visible:
        # Show Settings -> Mouse Visible
        Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
    else:
        # Hide Settings -> Restore Capture
        Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func toggle_mode():
    if current_mode == PlayMode.WALK:
        current_mode = PlayMode.STREAM_CONTROL
        # Stay captured for stream control usually, unless using absolute mouse
        print("Switched to STREAM CONTROL mode")
    else:
        current_mode = PlayMode.WALK
        print("Switched to WALK mode")

func handle_walk_input(event: InputEvent):
    # Mouse Look
    if event is InputEventMouseMotion and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
        head.rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
        player.rotate_y(-event.relative.x * MOUSE_SENSITIVITY) # Body rotates Y
        
        # Head (Camera) rotates X
        var camera = head.get_node("Camera3D")
        camera.rotate_x(-event.relative.y * MOUSE_SENSITIVITY)
        camera.rotation.x = clamp(camera.rotation.x, deg_to_rad(-89), deg_to_rad(89))
        head.rotation.y = 0 # Keep head aligned

    # Click to capture if somehow lost and panel is closed
    if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        if not settings_panel.visible and Input.get_mouse_mode() == Input.MOUSE_MODE_VISIBLE:
            Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func handle_stream_input(event: InputEvent):
    if not input_enabled:
        return
        
    # Forward Input to Moonlight
    if event is InputEventMouseMotion:
        # Send relative motion
        moonlightstreamcore.send_mouse_move_event(int(event.relative.x), int(event.relative.y))
        
    if event is InputEventMouseButton:
        var action = MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_PRESS if event.pressed else MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_RELEASE
        var button = 0
        if event.button_index == MOUSE_BUTTON_LEFT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_LEFT
        elif event.button_index == MOUSE_BUTTON_RIGHT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_RIGHT
        elif event.button_index == MOUSE_BUTTON_MIDDLE: button = MoonlightInput.MouseButton.MOUSE_BUTTON_MIDDLE
        elif event.button_index == MOUSE_BUTTON_WHEEL_UP:
            # Scroll is often handled differently or via dedicated scroll event if available
            pass
        elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
            pass
        # Note: add X1/X2 buttons if needed
        
        if button != 0:
            moonlightstreamcore.send_mouse_button_event(action, button)

    if event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.ctrl_pressed: modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.shift_pressed: modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.alt_pressed: modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed: modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        # The backend now handles mapping Godot keycodes to standard HID keycodes.
        # We can pass the Godot keycode directly without any manual conversion here.
        var keycode = event.keycode
        moonlightstreamcore.send_keyboard_event(keycode, action, modifiers)

func _physics_process(delta: float) -> void:
    # Stop player movement if panel is open or not in WALK mode
    if settings_panel.visible or current_mode != PlayMode.WALK:
        return
        
    # Add gravity
    if not player.is_on_floor():
        player.velocity += player.get_gravity() * delta

    # Handle Jump
    if (Input.is_action_just_pressed("ui_accept") or mobile_jump_requested) and player.is_on_floor():
        player.velocity.y = JUMP_VELOCITY
    mobile_jump_requested = false

    # Get input direction
    var input_dir = Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
    if is_mobile_platform:
        input_dir += move_joystick_vector
        if input_dir.length() > 1.0:
            input_dir = input_dir.normalized()
    # Direction relative to Player body
    var direction = (player.transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()
    
    if direction:
        player.velocity.x = direction.x * SPEED
        player.velocity.z = direction.z * SPEED
    else:
        player.velocity.x = move_toward(player.velocity.x, 0, SPEED)
        player.velocity.z = move_toward(player.velocity.z, 0, SPEED)

    player.move_and_slide()

func _process(delta: float) -> void:
    if is_mobile_platform:
        update_mobile_controls_layout()
        apply_mobile_camera_look(delta)

func apply_mobile_camera_look(delta: float) -> void:
    if settings_panel.visible:
        return
    if joystick_vector == Vector2.ZERO:
        return

    player.rotate_y(-joystick_vector.x * JOYSTICK_LOOK_SPEED * delta)
    camera.rotate_x(-joystick_vector.y * JOYSTICK_LOOK_SPEED * delta)
    camera.rotation.x = clamp(camera.rotation.x, deg_to_rad(-89), deg_to_rad(89))

# --- Button Handlers ---

func toggle_fullscreen():
    var mode = DisplayServer.window_get_mode()
    if mode == DisplayServer.WINDOW_MODE_FULLSCREEN:
        DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
    else:
        DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN)

func toggle_audio_bypass():
    # 切换音频旁路系统运行状态：如果运行在系统后端，能直接提升实时性并脱离 Godot 播放器依赖
    if moonlightstreamcore.is_native_audio_bypass_running():
        moonlightstreamcore.stop_native_audio_bypass()
        print("原生音频旁路已停止")
        $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: OFF"
    else:
        if moonlightstreamcore.start_native_audio_bypass():
            print("原生音频旁路成功启动")
            $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON"
        else:
            print("原生音频旁路启动失败")

func toggle_pause_audio_bypass():
    # 暂停或恢复后端直接播放音频功能
    if moonlightstreamcore.is_native_audio_bypass_paused():
        moonlightstreamcore.resume_native_audio_bypass()
        print("原生音频旁路恢复播放")
    else:
        moonlightstreamcore.pause_native_audio_bypass()
        print("原生音频旁路已暂停播放")

func toggle_virtual_keyboard():
    print("Toggle Keyboard (Not Implemented)")

func set_godot_audio_enabled(enabled: bool) -> void:
    # 同步控制内置3D环绕系统的开关
    godot_audio_enabled = enabled
    for ap in audio_players:
        if not ap or not (ap is AudioStreamPlayer3D):
            continue
        if enabled:
            if ap.stream:
                ap.play()
        else:
            ap.stop()

# --- Helper Logic ---

func on_pair_complete(success, message):
    if success:
        print("[Moonlight-Godot-MoonlightComputerManager]", message)
    else:
        push_error("[Moonlight-Godot-MoonlightComputerManager]", message)

func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if toggled_on: return
    moonlight_config_manager.update_host(1, {"localaddress": line_edit_ip.text})

func test_http_requeset() -> void:
    var url = "http://httpbin.org/uuid"
    moonlight_requester.request(url, "GET", PackedByteArray(), {}, {}, Callable(self , "_on_request_completed"))

func _on_request_completed(code, body, headers, err):
    print("Request result: ", code)

func test_cert_create() -> void:
    print(moonlight_config_manager.get_client_keys())

func test_add_host_info() -> void:
    print("current hosts:\n", moonlight_config_manager.get_hosts(), "\n")

func test_remove_host_info() -> void:
    moonlight_config_manager.remove_host(1)

func test_start_pair() -> void:
    var pin = moonlight_computer_manager.start_pair(line_edit_ip.text)
    print("pin:", pin)

func test_cancel_pair() -> void:
    moonlight_computer_manager.unpair(1)

func test_get_applist() -> void:
    moonlight_computer_manager.get_app_list(1)

func test_get_app_texture() -> void:
    moonlight_config_manager.load_config()
    var applist = moonlight_config_manager.get_apps(1)
    if applist.size() > 0:
        moonlight_computer_manager.get_app_cover(1, applist[0]["id"], func(tex): texture_rect_app.texture = tex)

func test_connect_to_server() -> void:
    moonlight_computer_manager.connect_to_computer(line_edit_ip.text, 47989, func(info): print(info))

# -----------------
# 串流生命周期及音频挂载核心 (重点参考)
# -----------------

func test_establish_stream() -> void:
    # 从 UI 界面更新所有参数标记
    if grid_container.has_node("ToggleVideoButton"):
        video_enabled = grid_container.get_node("ToggleVideoButton").button_pressed
    if grid_container.has_node("ToggleAudioButton"):
        audio_enabled = grid_container.get_node("ToggleAudioButton").button_pressed
    if grid_container.has_node("ToggleInputButton"):
        input_enabled = grid_container.get_node("ToggleInputButton").button_pressed
    if grid_container.has_node("CheckButton2"):
        godot_audio_enabled = grid_container.get_node("CheckButton2").button_pressed

    # 1. 使用 Resource API 构建串流的核心描述（比特率、帧率、视口比例大小）
    var cfg = MoonlightStreamConfigurationResource.new()
    cfg.set_width(1920)
    cfg.set_height(1080)
    cfg.set_fps(60)
    if line_edit_bitrate.text.is_valid_float():
        cfg.set_bitrate(int(float(line_edit_bitrate.text) * 1000))
    else:
        cfg.set_bitrate(20000)
    
    # 启用七声道环绕立体声（与 3D 渲染适配最佳）
    cfg.set_audio_configuration(MoonlightStreamConfigurationResource.AUDIO_CFG_71_SURROUND)

    # 2. 从界面下拉框选配硬件加速和视频协议
    var add_opts = MoonlightAdditionalStreamOptions.new()
    if grid_container.has_node("OptionButton"):
        add_opts.set_video_codec(grid_container.get_node("OptionButton").get_selected_id())
    if grid_container.has_node("CheckButton"):
        add_opts.set_disable_hw_acceleration(not grid_container.get_node("CheckButton").button_pressed)
    
    # 选项同步：如果 disable_audio 为 true，底层将告知主机端不传输任何音频流
    add_opts.set_disable_video(not video_enabled)
    add_opts.set_disable_audio(not audio_enabled)

    # 3. 将模型中介用于接收输出的视口中含的 TextureRect 配置给内核
    if video_enabled:
        moonlightstreamcore.set_render_target(stream_texture)
    
    # 4. 指定 ID 对应的电脑和对应的 App 发送启动请求
    moonlightstreamcore.start_play_stream(1, 1191261554, cfg, add_opts)
    
    # 5. 音频初始化（必须延迟以确保连接完成通道协商）
    if audio_enabled:
        await get_tree().create_timer(1.0).timeout
        
        # 对于 7.1 环境或者立体声等，后端会自动拆拆分轨道并返回多流列表
        var streams = moonlightstreamcore.get_audio_streams()
        
        # 遍历场景中所安插的声音发射节点给其分别绑定对应返回的声轨 AudioStream 对象
        for i in range(audio_players.size()):
            var ap = audio_players[i]
            if not ap or not (ap is AudioStreamPlayer3D):
                continue
            if i < streams.size():
                ap.stream = streams[i]
                if godot_audio_enabled:
                    ap.play()
                else:
                    ap.stop()
                print("Assigned audio stream to player:", ap.name)
            else:
                # 若无法映射足够声道则清理
                ap.stream = null
                ap.stop()
        if streams.size() == 0:
            print("No audio streams received from moonlightstreamcore")
    else:
        # 如果未勾选音频，静默化所有相关播放装置
        for ap in audio_players:
            if ap and (ap is AudioStreamPlayer3D):
                ap.stop()

    # 安全锁：确保在最后依然对网格映射视口的绘制执行强验证
    if video_enabled:
        var mat = screen_mesh.material_override
        if mat: mat.albedo_texture = stream_viewport.get_texture()

func test_stop_stream() -> void:
    moonlight_computer_manager.stop_stream(1, func(info):
        print("[Moonlight-Godot] Stop Stream: ", info)
        
        # 在退出时必须进行状态释放处理以避免悬挂野指针抛出异常和崩溃
        moonlightstreamcore.stop_play_stream()
        moonlightstreamcore.reset_audio_stream()
        
        # 即时暂停旁路声音（如果还在执行的话）
        if moonlightstreamcore.is_native_audio_bypass_running():
            moonlightstreamcore.stop_native_audio_bypass()
            
        moonlightstreamcore.reset_render_target()
        for ap in audio_players:
            if ap and (ap is AudioStreamPlayer3D):
                ap.stop()
    )

func test_pause_stream() -> void:
    moonlightstreamcore.stop_play_stream()
    moonlightstreamcore.reset_audio_stream()
    moonlightstreamcore.reset_render_target()
    for ap in audio_players:
        if ap and (ap is AudioStreamPlayer3D):
            ap.stop()

func view_data() -> void:
    if scroll_container.has_node("FileDialog"):
        scroll_container.get_node("FileDialog").visible = true
