extends Node

# -----------------
# Moonlight 多重串流实例类封装
# -----------------
class MoonlightInstance:
    var index: int = 0
    
    # 每一个实例包含独立配置控制中心、网络连接与视频核心解析环境
    var moonlight_config_manager: MoonlightConfigManager
    var moonlight_computer_manager: MoonlightComputerManager
    var moonlightstreamcore: MoonlightStreamCore
    
    # 本地用于展现这个被串流主机输出数据的承载体
    var audio_player: AudioStreamPlayer
    var texture_rect: TextureRect
    var custom_config_path: String

# 所有注册并正在运行的 Moonlight 实例合集
var instances: Array[MoonlightInstance] = []
# 当前用于展示、操作的活动实例对象
var current_instance: MoonlightInstance = null

@onready var moonlight_requester = MoonlightRequester.new()

var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var is_fullscreen: bool = false
var vk_modifiers: int = 0

func _ready() -> void:
    _setup_virtual_keyboard()
    _add_instance() # 初始化默认的第一个串流主机实例

func _add_instance() -> void:
    # --- 新增独立串流实例运行沙盒 ---
    var inst = MoonlightInstance.new()
    inst.index = instances.size()
    
    # 为每一个新的实例分离存储配置空间，避免数据和配对密钥相互污染覆盖
    if inst.index == 0:
        inst.custom_config_path = ""
    else:
        inst.custom_config_path = "user://addons/moonlight-godot/multiconfig/config%s.ini" % inst.index
        
    inst.moonlight_config_manager = MoonlightConfigManager.new()
    inst.moonlight_computer_manager = MoonlightComputerManager.new()
    inst.moonlightstreamcore = MoonlightStreamCore.new()
    
    # 手动重定向配置加载路径并做绑定下发
    if inst.custom_config_path != "":
        inst.moonlight_config_manager.config_path = inst.custom_config_path
    inst.moonlight_computer_manager.set_config_manager(inst.moonlight_config_manager)
    inst.moonlightstreamcore.set_config_manager(inst.moonlight_config_manager)
    
    # 增加状态监听反馈日志
    inst.moonlight_computer_manager.pair_completed.connect(on_pair_complete)
    inst.moonlightstreamcore.connection_started.connect(func(): print("[Instance %s] Connect Successfully" % inst.index))
    inst.moonlightstreamcore.connection_terminated.connect(func(_err, msg): push_error("[Instance %s] " % inst.index, msg))
    
    # 动态为它在 UI 排版建立对应的声音控制器
    var audio_p = AudioStreamPlayer.new()
    audio_p.autoplay = true
    $ScrollContainer/GridContainer.add_child(audio_p)
    inst.audio_player = audio_p
    
    # 然后再为其分配一个特定的屏幕显示块 (分辨率随意，保证 Aspect 设置为保持就好)
    var tex_r = TextureRect.new()
    tex_r.custom_minimum_size = Vector2(800, 450)
    tex_r.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
    tex_r.gui_input.connect(func(event): _on_instance_screen_gui_input(event, inst))
    $ScrollContainer/GridContainer/ScreensGrid.add_child(tex_r)
    inst.texture_rect = tex_r
    
    instances.append(inst)
    
    # 同步选项到多路切换的 OptionButton 组件上
    var opt_btn = $ScrollContainer/GridContainer/InstanceOption
    opt_btn.add_item("Instance " + str(inst.index))
    
    # 初次增加，或者唯一存在的时候即作为 Current 环境
    if current_instance == null:
        _select_instance(0)

func _on_add_instance_btn_pressed() -> void:
    _add_instance()

func _on_instance_option_item_selected(index: int) -> void:
    # 切换目标焦点（将控制区域指令转发给这个编号的子对象去执行操作）
    _select_instance(index)

func _select_instance(index: int) -> void:
    if index >= 0 and index < instances.size():
        current_instance = instances[index]
        
        # 刷新下拉按钮状态
        var opt_btn = $ScrollContainer/GridContainer/InstanceOption
        opt_btn.selected = index
        
        # 提取当前主机的局域网地址恢复到输入框内
        var hosts = current_instance.moonlight_config_manager.get_hosts()
        if hosts.size() > 0:
            $ScrollContainer/GridContainer/LineEdit.text = hosts[0].localaddress

func on_pair_complete(success, message):
    if success: print("[Moonlight-Godot] ", message)
    else: push_error("[Moonlight-Godot] ", message)

# -----------------
# 针对当前切出控制实例的方法装饰包装 (Helper for delegating to current instance core objects)
# 以确保全局代码依然和普通的单实例具有同样的直观调用格式，方便维护
# -----------------

func _moonlight() -> MoonlightStreamCore:
    if current_instance: return current_instance.moonlightstreamcore
    return null

func _computer() -> MoonlightComputerManager:
    if current_instance: return current_instance.moonlight_computer_manager
    return null

func _config() -> MoonlightConfigManager:
    if current_instance: return current_instance.moonlight_config_manager
    return null

# -----------------
# 各项状态控制流程
# -----------------

func test_http_requeset() -> void:
    # 请求验证，网络接口探测
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
    # 对应的串流控制环境分配专属的配对请求信息通道
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
    # 请求发起串流（利用 _moonlight() 等当前活动包裹器）
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

    # 这里不再用统一的 UI 组件而是使用绑在这个实例上的 texture_rect 用于只渲染他自己的游戏流数据
    if video_enabled:
        _moonlight().set_render_target(current_instance.texture_rect)
        
    # 同理分配到它专门的声音流播放器上，实现完全隔离互不干涉
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
        # 清除绑定的 UI 结点引用，避免内存泄漏或者是画面冻结旧帧导致黑屏或者残影报错
        _moonlight().reset_render_target()

func view_data() -> void:
    $ScrollContainer/FileDialog.visible = true

func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if _config() and not toggled_on:
        _config().update_host(1, {"localaddress": $ScrollContainer/GridContainer/LineEdit.text})

# -----------------
# 音频旁路与界面事件
# -----------------

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

    # 获取分发本实例的光标偏移进行独立发送
    if event is InputEventMouseMotion:
        mc.send_mouse_position_event(int(event.position.x), int(event.position.y), screen_width, screen_height)
        
    # 分发按钮动作
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
            
    # 全局实体键盘按键统一拦截并分配到聚焦中的主窗体
    elif event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.shift_pressed: modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.ctrl_pressed: modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.alt_pressed: modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed: modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        # MoonlightCore 模块后台已经封装了从 Godot keycode 到对应 HID Keycode 的映射处理，可直接传递。
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
