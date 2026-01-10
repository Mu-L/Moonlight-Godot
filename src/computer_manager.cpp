#include "computer_manager.h"
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

ComputerManager::ComputerManager() {
	requester = memnew(Requester);
}

ComputerManager::~ComputerManager() {
	if (requester) {
		memdelete(requester);
	}
}

void ComputerManager::set_config_manager(Object *cm) {
	config_manager = Object::cast_to<ConfigManager>(cm);
}

// ============================================================================
// 1. 配对逻辑
// ============================================================================

String ComputerManager::start_pair(String ip, int port) {
	_reset_pairing();
	pair_ip = ip;
	pair_port = port;
	pair_https_port = cached_https_ports.get(ip, 47984);

	// 1. 生成PIN码（4位随机数字）
	int pin_val = UtilityFunctions::randi() % 10000;
	pair_pin = String::num_int64(pin_val).pad_zeros(4);

	// 2. 生成盐和密钥
	pair_salt = _generate_random_bytes(16);
	pair_aes_key = _calculate_aes_key(pair_salt, pair_pin);

	// 3. 初始化状态
	pair_state = PAIR_GET_CERT;
	is_requesting = false;
	unique_id = _get_unique_id();

	return pair_pin;
}

String ComputerManager::complete_pair() {
	if (pair_state == PAIR_IDLE)
		return "idle";
	if (pair_state == PAIR_FINISHED)
		return "paired";
	if (pair_state == PAIR_ERROR)
		return "failed";
	if (is_requesting)
		return "working";

	String base_url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/pair";
	String common_params = "uniqueid=" + unique_id + "&uuid=" + _get_uuid() + "&devicename=roth&updateState=1";

	Dictionary ssl_opts;

	switch (pair_state) {
		case PAIR_GET_CERT: {
			Dictionary keys = config_manager->get_client_keys();
			String client_cert_pem = keys["certificate"];
			// 简单清理 PEM 以便传输头部（移除头部/换行符）
			String cert_clean = client_cert_pem.replace("-----BEGIN CERTIFICATE-----", "").replace("-----END CERTIFICATE-----", "").replace("\n", "");
			PackedByteArray cert_bytes = Marshalls::get_singleton()->base64_to_raw(cert_clean);

			String url = base_url + "?" + common_params + "&phrase=getservercert&salt=" + _bytes_to_hex(pair_salt) + "&clientcert=" + _bytes_to_hex(cert_bytes);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(1));
			is_requesting = true;
			break;
		}
		case PAIR_CLIENT_CHALLENGE: {
			PackedByteArray random_bytes = _generate_random_bytes(16);
			PackedByteArray challenge_enc = _encrypt_aes_ecb(random_bytes, pair_aes_key);

			String url = base_url + "?" + common_params + "&clientchallenge=" + _bytes_to_hex(challenge_enc);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(2));
			is_requesting = true;
			break;
		}
		case PAIR_SERVER_CHALLENGE_RESP: {
			// 有效载荷存储在上一步回调的 last_error 中
			String payload = last_error;
			last_error = "";
			String url = base_url + "?" + common_params + "&serverchallengeresp=" + payload;
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(3));
			is_requesting = true;
			break;
		}
		case PAIR_CLIENT_PAIRING_SECRET: {
			String payload = last_error;
			last_error = "";
			String url = base_url + "?" + common_params + "&clientpairingsecret=" + payload;
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(4));
			is_requesting = true;
			break;
		}
		case PAIR_HTTPS_PAIR_CHALLENGE: {
			String https_url = "https://" + pair_ip + ":" + String::num_int64(pair_https_port) + "/pair";
			String url = https_url + "?" + common_params + "&phrase=pairchallenge";
			requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(), callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(5));
			is_requesting = true;
			break;
		}
	}

	return "working";
}

void ComputerManager::_on_pair_request_completed(int code, PackedByteArray body, Dictionary headers, String error, int step) {
	is_requesting = false;

	if (code != 200) {
		if (step == 1)
			return; // 仍在等待主机输入PIN码
		pair_state = PAIR_ERROR;
		return;
	}

	String xml = body.get_string_from_utf8();
	if (_extract_xml_value(xml, "paired") != "1") {
		if (step == 1)
			return; // 仍在等待
		pair_state = PAIR_ERROR;
		return;
	}

	switch (step) {
		case 1: { // Get Cert
			String plaincert = _extract_xml_value(xml, "plaincert");
			if (!plaincert.is_empty()) {
				// 响应中的 plaincert 是 Hex 编码的 PEM 证书
				server_cert_pem = _hex_to_bytes(plaincert).get_string_from_ascii();
				pair_state = PAIR_CLIENT_CHALLENGE;
			}
			break;
		}
		case 2: { // Got Server Response
			String resp_hex = _extract_xml_value(xml, "challengeresponse");
			PackedByteArray resp = _decrypt_aes_ecb(_hex_to_bytes(resp_hex), pair_aes_key);

			// 简化逻辑：对解密后的响应进行哈希处理并重新加密
			PackedByteArray hash = _sha256(resp);
			PackedByteArray hash_enc = _encrypt_aes_ecb(hash, pair_aes_key);
			last_error = _bytes_to_hex(hash_enc); // 进入下一步

			pair_state = PAIR_SERVER_CHALLENGE_RESP;
			break;
		}
		case 3: { // 获取配对密钥
			// 准备第四阶段
			PackedByteArray client_secret = _generate_random_bytes(16);
			PackedByteArray signature = _sign_data(client_secret);
			PackedByteArray payload = client_secret;
			payload.append_array(signature);

			last_error = _bytes_to_hex(payload);
			pair_state = PAIR_CLIENT_PAIRING_SECRET;
			break;
		}
		case 4: {
			pair_state = PAIR_HTTPS_PAIR_CHALLENGE;
			break;
		}
		case 5: {
			// 成功
			Dictionary host_data;
			host_data["hostname"] = pair_ip;
			host_data["localaddress"] = pair_ip;
			host_data["uuid"] = _get_uuid();
			host_data["srvcert"] = server_cert_pem;
			host_data["https_port"] = pair_https_port;
			config_manager->add_host(host_data);
			pair_state = PAIR_FINISHED;
			break;
		}
	}
}

void ComputerManager::cancel_pair() {
	if (pair_state != PAIR_IDLE) {
		String url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/unpair?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
		requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(), Callable());
	}
	_reset_pairing();
}

void ComputerManager::unpair(int host_id) {
	config_manager->remove_host(host_id);
}

void ComputerManager::_reset_pairing() {
	pair_state = PAIR_IDLE;
	is_requesting = false;
	last_error = "";
	server_cert_pem = "";
}

// ============================================================================
// 2. 连接
// ============================================================================

void ComputerManager::connect_to_computer(String ip, int port, Callable callback) {
	String url = "http://" + ip + ":" + String::num_int64(port) + "/serverinfo?uniqueid=" + _get_unique_id() + "&uuid=" + _get_uuid();
	requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(),
			callable_mp(this, &ComputerManager::_on_server_info_completed).bind(Variant(callback), Variant(ip)));
}

void ComputerManager::_on_server_info_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback, String ip) {
	Dictionary result;
	result["status"] = (code == 200) ? "online" : "offline";
	if (code == 200) {
		String xml = body.get_string_from_utf8();
		result["hostname"] = _extract_xml_value(xml, "hostname");
		result["paired"] = _extract_xml_value(xml, "PairStatus") == "1";
		result["ip"] = ip;

		String port_str = _extract_xml_value(xml, "HttpsPort");
		int port = port_str.is_empty() ? 47984 : port_str.to_int();
		result["https_port"] = port;
		cached_https_ports[ip] = port;
	}
	if (callback.is_valid())
		callback.call(result);
}

// ============================================================================
// 3. 应用列表
// ============================================================================

void ComputerManager::get_app_list(int host_id, Callable callback) {
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}
	if (ip.is_empty())
		return;

	String url = "https://" + ip + ":" + String::num_int64(port) + "/applist?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &ComputerManager::_on_app_list_completed).bind(Variant(host_id), Variant(callback)));
}

void ComputerManager::_on_app_list_completed(int code, PackedByteArray body, Dictionary headers, String error, int host_id, Callable callback) {
	if (code == 200) {
		String xml = body.get_string_from_utf8();
		int pos = 0;
		while (true) {
			int start = xml.find("<App>", pos);
			if (start == -1)
				break;
			int end = xml.find("</App>", start);
			if (end == -1)
				break;

			String app_xml = xml.substr(start, end - start + 6);
			Dictionary app_data;
			app_data["name"] = _extract_xml_value(app_xml, "AppTitle");
			app_data["id"] = _extract_xml_value(app_xml, "ID").to_int();
			config_manager->add_app(host_id, app_data);
			pos = end + 6;
		}
	}
	if (callback.is_valid())
		callback.call(code == 200);
}

void ComputerManager::get_app_cover(int host_id, int app_id, Callable callback) {
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}
	if (ip.is_empty())
		return;

	String url = "https://" + ip + ":" + String::num_int64(port) + "/appasset?uniqueid=" + unique_id + "&uuid=" + _get_uuid() + "&appid=" + String::num_int64(app_id);
	requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &ComputerManager::_on_app_cover_completed).bind(Variant(callback)));
}

void ComputerManager::_on_app_cover_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback) {
	if (code == 200 && callback.is_valid()) {
		Ref<Image> img;
		img.instantiate();
		if (img->load_png_from_buffer(body) == OK || img->load_jpg_from_buffer(body) == OK) {
			Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
			callback.call(tex);
			return;
		}
	}
	if (callback.is_valid())
		callback.call(Variant());
}

// ============================================================================
// 4. Stream
// ============================================================================

void ComputerManager::establish_stream(int host_id, int app_id, Dictionary options, Callable callback) {
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}

	String url = "https://" + ip + ":" + String::num_int64(port) + "/launch?uniqueid=" + unique_id + "&uuid=" + _get_uuid() + "&appid=" + String::num_int64(app_id);
	url += "&mode=" + String::num_int64(options.get("width", 1280)) + "x" + String::num_int64(options.get("height", 720)) + "x" + String::num_int64(options.get("fps", 60));

	requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &ComputerManager::_on_simple_request_completed).bind(Variant(callback)));
}

void ComputerManager::stop_stream(int host_id, Callable callback) {
	Array hosts = config_manager->get_hosts();
	String ip;
	int port = 47984;
	for (int i = 0; i < hosts.size(); i++) {
		Dictionary host = hosts[i];
		if ((int64_t)host["id"] == host_id) {
			ip = host.get("localaddress", "");
			port = host.get("https_port", 47984);
		}
	}

	String url = "https://" + ip + ":" + String::num_int64(port) + "/cancel?uniqueid=" + unique_id + "&uuid=" + _get_uuid();
	requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(),
			callable_mp(this, &ComputerManager::_on_simple_request_completed).bind(Variant(callback)));
}

void ComputerManager::_on_simple_request_completed(int code, PackedByteArray body, Dictionary headers, String error, Callable callback) {
	if (callback.is_valid())
		callback.call(code == 200 ? body.get_string_from_utf8() : "");
}

// ============================================================================
// 助手
// ============================================================================

PackedByteArray ComputerManager::_generate_random_bytes(int size) {
	Ref<Crypto> c;
	c.instantiate();
	return c->generate_random_bytes(size);
}

PackedByteArray ComputerManager::_calculate_aes_key(const PackedByteArray &salt, const String &pin) {
	PackedByteArray combined = salt;
	combined.append_array(pin.to_ascii_buffer());
	return _sha256(combined).slice(0, 16);
}

PackedByteArray ComputerManager::_encrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key) {
	Ref<AESContext> ctx;
	ctx.instantiate();
	ctx->start(AESContext::MODE_ECB_ENCRYPT, key);
	PackedByteArray out = ctx->update(data);
	ctx->finish();
	return out;
}

PackedByteArray ComputerManager::_decrypt_aes_ecb(const PackedByteArray &data, const PackedByteArray &key) {
	Ref<AESContext> ctx;
	ctx.instantiate();
	ctx->start(AESContext::MODE_ECB_DECRYPT, key);
	PackedByteArray out = ctx->update(data);
	ctx->finish();
	return out;
}

PackedByteArray ComputerManager::_sha256(const PackedByteArray &data) {
	Ref<HashingContext> ctx;
	ctx.instantiate();
	ctx->start(HashingContext::HASH_SHA256);
	ctx->update(data);
	return ctx->finish();
}

PackedByteArray ComputerManager::_sign_data(const PackedByteArray &data) {
	Dictionary keys = config_manager->get_client_keys();
	Ref<Crypto> c;
	c.instantiate();
	Ref<CryptoKey> key;
	key.instantiate();
	if (key->load_from_string(keys["key"]) != OK)
		return PackedByteArray();
	return c->sign(HashingContext::HASH_SHA256, data, key);
}

String ComputerManager::_bytes_to_hex(const PackedByteArray &bytes) {
	return bytes.hex_encode().to_lower();
}

PackedByteArray ComputerManager::_hex_to_bytes(const String &hex) {
	return hex.hex_decode();
}

String ComputerManager::_extract_xml_value(const String &xml, const String &tag) {
	String start_tag = "<" + tag + ">";
	String end_tag = "</" + tag + ">";
	int start = xml.find(start_tag);
	if (start == -1)
		return "";
	start += start_tag.length();
	int end = xml.find(end_tag, start);
	if (end == -1)
		return "";
	return xml.substr(start, end - start);
}

String ComputerManager::_get_unique_id() {
	if (config_manager->get_custom_data(ConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", "").stringify().is_empty()) {
		String uid = _bytes_to_hex(_generate_random_bytes(8)).to_upper();
		config_manager->set_custom_data(ConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", uid);
	}
	return config_manager->get_custom_data(ConfigManager::TARGET_GLOBAL, 0, 0, "uniqueid", "").stringify();
}

String ComputerManager::_get_uuid() {
	return _bytes_to_hex(_generate_random_bytes(16));
}

Dictionary ComputerManager::_get_ssl_options() {
	Dictionary d;
	Dictionary keys = config_manager->get_client_keys();
	d["client_cert"] = keys["certificate"];
	d["client_key"] = keys["key"];
	return d;
}

void ComputerManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_config_manager", "cm"), &ComputerManager::set_config_manager);

	ClassDB::bind_method(D_METHOD("start_pair", "ip", "port"), &ComputerManager::start_pair, DEFVAL(47989));
	ClassDB::bind_method(D_METHOD("complete_pair"), &ComputerManager::complete_pair);
	ClassDB::bind_method(D_METHOD("cancel_pair"), &ComputerManager::cancel_pair);
	ClassDB::bind_method(D_METHOD("unpair", "host_id"), &ComputerManager::unpair);

	ClassDB::bind_method(D_METHOD("connect_to_computer", "ip", "port", "callback"), &ComputerManager::connect_to_computer, DEFVAL(47989), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("get_app_list", "host_id", "callback"), &ComputerManager::get_app_list, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("get_app_cover", "host_id", "app_id", "callback"), &ComputerManager::get_app_cover);
	ClassDB::bind_method(D_METHOD("establish_stream", "host_id", "app_id", "options", "callback"), &ComputerManager::establish_stream);
	ClassDB::bind_method(D_METHOD("stop_stream", "host_id", "callback"), &ComputerManager::stop_stream);

	ClassDB::bind_method(D_METHOD("_on_pair_request_completed"), &ComputerManager::_on_pair_request_completed);
	ClassDB::bind_method(D_METHOD("_on_server_info_completed"), &ComputerManager::_on_server_info_completed);
	ClassDB::bind_method(D_METHOD("_on_app_list_completed"), &ComputerManager::_on_app_list_completed);
	ClassDB::bind_method(D_METHOD("_on_app_cover_completed"), &ComputerManager::_on_app_cover_completed);
	ClassDB::bind_method(D_METHOD("_on_simple_request_completed"), &ComputerManager::_on_simple_request_completed);
}
