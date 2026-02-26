extends Node

@onready var requester = Requester.new()
@onready var configmanager = ConfigManager.new()
@onready var computermamager = ComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

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

    var add_opts = MoonlightAdditionalStreamOptions.new()
    add_opts.set_video_codec($ScrollContainer/GridContainer/OptionButton.get_selected_id())
    add_opts.set_disable_hw_acceleration(not $ScrollContainer/GridContainer/CheckButton.button_pressed)

    moonlightstreamcore.set_render_target($ScrollContainer/GridContainer/Screen)
    $ScrollContainer/GridContainer/AudioStreamPlayer.stream = moonlightstreamcore.get_audio_stream()

    # 直接开始播放（host_id 与 app_id 仍使用示例中的固定值）
    moonlightstreamcore.start_play_stream(1, 1191261554, cfg, add_opts)
    await get_tree().create_timer(1).timeout
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
