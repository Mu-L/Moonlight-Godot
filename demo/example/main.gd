extends Node2D


func test_http_requeset() -> void:
    var requester = Requester.new()
    var url = "http://httpbin.org/uuid"
    var method = "GET"
    var body = PackedByteArray()
    var headers = {}
    var ssl_options = {} # 不附加证书
    var callback = Callable(self, "_on_baidu_request_completed")
    
    requester.request(url, method, body, headers, ssl_options, callback)

func _on_baidu_request_completed(response_code: int, response_body: PackedByteArray, response_headers: Dictionary, error_text: String) -> void:
    if error_text.is_empty():
        print("请求成功，状态码: ", response_code)
        print("响应内容: ", response_body.get_string_from_utf8())
    else:
        print("请求失败: ", error_text)


func test_cert_create() -> void:
    var configmanager = ConfigManager.new()
    print(configmanager.get_client_keys())


func test_add_host_info() -> void:
    var config: Dictionary={
            "hostname":"TEST",
            "uuid":Time.get_datetime_string_from_system()
        }
    var configmanager = ConfigManager.new()
    print(configmanager.add_host(config))
    print("current hosts:\n")
    print(configmanager.get_hosts(),"\n")


func test_remove_host_info() -> void:
    var configmanager = ConfigManager.new()
    var size = configmanager.get_hosts().size()
    configmanager.remove_host(1)
