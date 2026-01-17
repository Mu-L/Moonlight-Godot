extends Node

@onready var requester = Requester.new()
@onready var configmanager = ConfigManager.new()
@onready var computermamager = ComputerManager.new()

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
    var config: Dictionary={
            "hostname":"TEST",
            "uuid":Time.get_datetime_string_from_system()
        }
    print(configmanager.add_host(config))
    print("current hosts:\n")
    print(configmanager.get_hosts(),"\n")


func test_remove_host_info() -> void:
    @warning_ignore("unused_variable")
    var size = configmanager.get_hosts().size()
    configmanager.remove_host(1)


func test_start_pair() -> void:
    var pin = computermamager.start_pair("127.0.0.1")
    print("pin:",pin)

func test_cancel_pair() -> void:
    computermamager.unpair(1)
