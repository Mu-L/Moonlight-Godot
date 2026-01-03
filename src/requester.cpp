#include "requester.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <vector>

using namespace godot;

static std::once_flag curl_init_flag;

Requester::Requester() {
	// 确保 curl 全局初始化只执行一次，且线程安全
	std::call_once(curl_init_flag, []() {
		curl_global_init(CURL_GLOBAL_ALL);
	});
}

Requester::~Requester() {
}

void Requester::_bind_methods() {
	ClassDB::bind_method(D_METHOD("request", "url", "method", "body", "headers", "ssl_options", "callback"), &Requester::request);
}

size_t Requester::_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t real_size = size * nmemb;
	std::vector<uint8_t> *mem = (std::vector<uint8_t> *)userp;
	mem->insert(mem->end(), (uint8_t *)contents, (uint8_t *)contents + real_size);
	return real_size;
}

size_t Requester::_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	size_t real_size = size * nitems;
	Dictionary *headers = (Dictionary *)userdata;
	String header_line = String::utf8(buffer, real_size).strip_edges();

	int split_idx = header_line.find(":");
	if (split_idx > 0) {
		String key = header_line.substr(0, split_idx).strip_edges();
		String value = header_line.substr(split_idx + 1).strip_edges();
		(*headers)[key] = value;
	}
	return real_size;
}

void Requester::request(String p_url, String p_method, PackedByteArray p_body, Dictionary p_headers, Dictionary p_ssl_options, Callable p_callback) {
	// 使用 std::thread 异步执行，detach 分离线程（简单起见，生产环境建议使用线程池）
	std::thread([=]() {
		_perform_request_thread(p_url, p_method, p_body, p_headers, p_ssl_options, p_callback);
	}).detach();
}

void Requester::_perform_request_thread(String p_url, String p_method, PackedByteArray p_body, Dictionary p_headers, Dictionary p_ssl_options, Callable p_callback) {
	CURL *curl = curl_easy_init();
	ResponseData res_data;
	std::vector<uint8_t> body_buffer;
	struct curl_slist *chunk = nullptr;

	if (curl) {
		std::string url_std = p_url.utf8().get_data();
		std::string method_std = p_method.to_upper().utf8().get_data();

		curl_easy_setopt(curl, CURLOPT_URL, url_std.c_str());

		// Method & Body Handling (Only GET/POST)
		if (method_std == "GET") {
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
		} else if (method_std == "POST") {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			if (p_body.size() > 0) {
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, p_body.ptr());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)p_body.size());
			} else {
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
			}
		} else {
			// Unsupported method
			res_data.error_text = "Unsupported method: " + p_method + ". Only GET and POST are supported.";
			res_data.response_code = -1;
			curl_easy_cleanup(curl);
			p_callback.call_deferred(res_data.response_code, res_data.body, res_data.headers, res_data.error_text);
			return;
		}

		// Headers
		Array keys = p_headers.keys();
		for (int i = 0; i < keys.size(); i++) {
			String key = keys[i];
			String val = p_headers[key];
			String header = key + ": " + val;
			chunk = curl_slist_append(chunk, header.utf8().get_data());
		}
		if (chunk)
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

		// MTLS / HTTPS Options
		// 客户端证书
		if (p_ssl_options.has("client_cert")) {
			String cert_path = p_ssl_options["client_cert"];
			if (!cert_path.is_empty()) {
				curl_easy_setopt(curl, CURLOPT_SSLCERT, cert_path.utf8().get_data());
				curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
			}
		}
		// 客户端密钥
		if (p_ssl_options.has("client_key")) {
			String key_path = p_ssl_options["client_key"];
			if (!key_path.is_empty()) {
				curl_easy_setopt(curl, CURLOPT_SSLKEY, key_path.utf8().get_data());
			}
		}
		// 服务端证书固定 (CA Pinning)
		if (p_ssl_options.has("server_cert")) {
			String ca_path = p_ssl_options["server_cert"];
			if (!ca_path.is_empty()) {
				curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path.utf8().get_data());
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
			}
		} else {
			// 如果没指定，默认不严格校验，或依赖系统CA（视需求而定）
			// 这里假设没传server_cert则不强制pinning，但libcurl默认会校验系统CA
		}

		// Callback setup
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buffer);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, _header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res_data.headers);

		// Perform
		CURLcode res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			res_data.error_text = String("Curl error: ") + curl_easy_strerror(res);
			res_data.response_code = -1;
		} else {
			long response_code;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
			res_data.response_code = (int)response_code;

			res_data.body.resize(body_buffer.size());
			if (body_buffer.size() > 0) {
				memcpy(res_data.body.ptrw(), body_buffer.data(), body_buffer.size());
			}
		}

		curl_easy_cleanup(curl);
		if (chunk)
			curl_slist_free_all(chunk);
	} else {
		res_data.error_text = "Failed to init curl";
		res_data.response_code = -1;
	}

	// 回到主线程执行回调
	// 签名: void(int code, PackedByteArray body, Dictionary headers, String error)
	p_callback.call_deferred(res_data.response_code, res_data.body, res_data.headers, res_data.error_text);
}
