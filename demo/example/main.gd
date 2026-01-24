extends Node

@onready var requester = Requester.new()
@onready var configmanager = ConfigManager.new()
@onready var computermamager = ComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

func _ready() -> void:
    computermamager.pair_completed.connect(on_pair_complete)

func on_pair_complete(success,message):
    if success:
        print("[Moonlight-Godot-ComputerManager]",message)
    else:
        push_error("[Moonlight-Godot-ComputerManager]",message)

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
    var pin = computermamager.start_pair("127.0.0.1")
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
    computermamager.connect_to_computer("127.0.0.1",47989,func(info): print(info))
    


func test_establish_stream() -> void:
    var options = {
        "width": "1920",
        "height": "1080",
        "fps": "60",
        "additionalStates": "1",
        "sops": "1",
        "surroundAudioInfo": "65536",
        "remoteControllersBitmap": "15",
        "gcmap": "1",
        "gcpersist": "1",
    }
    moonlightstreamcore.set_render_target($ScrollContainer/GridContainer/Screen)
    $ScrollContainer/GridContainer/AudioStreamPlayer.stream=moonlightstreamcore.get_audio_stream()
    computermamager.establish_stream(1,1191261554,options,func(info): print(info);moonlightstreamcore.start_play_stream(info))
    await get_tree().create_timer(2).timeout
    $ScrollContainer/GridContainer/AudioStreamPlayer.play()

func test_stop_stream() -> void:
    computermamager.stop_stream(1,func(info): print(info);moonlightstreamcore.stop_play_stream();moonlightstreamcore.reset_audio_stream();moonlightstreamcore.reset_render_target())
