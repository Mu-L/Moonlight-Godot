#pragma once

#include "moonlight_godot.h"
#include <curl/curl.h>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <mutex>
#include <string>
#include <thread>

namespace godot {

class Requester : public MoonlightGodot {
	GDCLASS(Requester, MoonlightGodot)

private:
	struct ResponseData {
		int response_code = 0;
		PackedByteArray body;
		Dictionary headers;
		String error_text;
	};

	static size_t _write_cb(void *contents, size_t size, size_t nmemb, void *userp);
	static size_t _header_cb(char *buffer, size_t size, size_t nitems, void *userdata);
	static void _perform_request_thread(String p_url, String p_method, PackedByteArray p_body, Dictionary p_headers, Dictionary p_ssl_options, Callable p_callback);

protected:
	static void _bind_methods();

public:
	Requester();
	~Requester();

	// url: 请求地址
	// method: 仅支持 "GET", "POST"
	// body: 请求体数据 (仅 POST 请求有效)
	// headers: 请求头字典
	// ssl_options: 包含 "client_cert", "client_key", "server_cert" (路径) 的字典
	// callback: 回调函数，签名 void(int code, PackedByteArray body, Dictionary headers, String error)
	void request(String p_url, String p_method, PackedByteArray p_body, Dictionary p_headers, Dictionary p_ssl_options, Callable p_callback);
};

} //namespace godot
