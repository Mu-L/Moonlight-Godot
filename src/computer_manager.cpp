#include "computer_manager.h"
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

ComputerManager::ComputerManager() {
	requester = memnew(Requester);
	owns_config_manager = false;
}

ComputerManager::~ComputerManager() {
	if (requester) {
		memdelete(requester);
	}
	if (owns_config_manager && config_manager) {
		memdelete(config_manager);
	}
}

void ComputerManager::set_config_manager(Object *cm) {
	config_manager = Object::cast_to<ConfigManager>(cm);
	owns_config_manager = false; // 外部所有权
}

// ============================================================================
// 1. 配对逻辑
// ============================================================================

String ComputerManager::start_pair(String ip, int port) {
	// 如果缺少 config_manager，则在内部初始化一个默认的
	if (config_manager == nullptr) {
		config_manager = memnew(ConfigManager);
		owns_config_manager = true;
		// ConfigManager 构造函数应处理加载默认值或空状态
	}

	_reset_pairing();
	pair_ip = ip;
	pair_port = port;
	pair_https_port = cached_https_ports.get(ip, 47984);
	unique_id = _get_unique_id();
	current_uuid = _get_uuid();

	// 1. 生成一个 PIN 码（4 位随机数字）
	int pin_val = UtilityFunctions::randi() % 10000;
	pair_pin = String::num_int64(pin_val).pad_zeros(4);

	// 2. Generate salt and key
	// client.c: SHA256(salt + PIN) -> aes_key (AES-128 uses 16 bytes)
	pair_salt = _generate_random_bytes(16);
	pair_aes_key = _calculate_aes_key(pair_salt, pair_pin);

	// 3. 开始进程
	pair_state = PAIR_STAGE_1_GET_CERT;
	_step_pair();

	return pair_pin;
}

void ComputerManager::_step_pair() {
	if (pair_state == PAIR_IDLE || pair_state == PAIR_FINISHED || pair_state == PAIR_ERROR)
		return;

	is_requesting = true;
	String base_url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/pair";
	String common_params = "uniqueid=" + unique_id + "&uuid=" + current_uuid + "&devicename=roth&updateState=1";

	Dictionary ssl_opts; // HTTP为空

	switch (pair_state) {
		case PAIR_STAGE_1_GET_CERT: {
			//Stage 1: Send the salt and client certificate, obtain the server certificate. The server blocks here, waiting for the PIN code input.
			Dictionary keys = config_manager->get_client_keys();
			String client_cert_pem = keys["certificate"];

			// Remove leading/trailing/line break characters for hexadecimal encoding
			String cert_clean = client_cert_pem.replace("-----BEGIN CERTIFICATE-----", "")
										.replace("-----END CERTIFICATE-----", "")
										.replace("\n", "")
										.replace("\r", "");

			PackedByteArray cert_bytes = Marshalls::get_singleton()->base64_to_raw(cert_clean);

			String url = base_url + "?" + common_params + "&phrase=getservercert&salt=" + _bytes_to_hex(pair_salt) + "&clientcert=" + _bytes_to_hex(cert_bytes);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(1));
			break;
		}
		case PAIR_STAGE_2_CLIENT_CHALLENGE: {
			// Stage 2: Send encrypted client challenge
			// Generate 16 bytes of random data
			client_secret_random = _generate_random_bytes(16);
			// Encrypt using AES key
			PackedByteArray challenge_enc = _encrypt_aes_ecb(client_secret_random, pair_aes_key);

			String url = base_url + "?" + common_params + "&clientchallenge=" + _bytes_to_hex(challenge_enc);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(2));
			break;
		}
		case PAIR_STAGE_3_SERVER_RESPONSE: {
			// 第3阶段：响应服务器挑战
			// 负载：ServerChallenge   CertSig   ClientSecret
			// 注意：之前的逻辑错误地使用了server_secret，应该使用server_challenge。

			// 1. 获取客户端证书 DER 以提取签名
			Dictionary keys = config_manager->get_client_keys();
			String cert_clean = String(keys["certificate"]).replace("-----BEGIN CERTIFICATE-----", "").replace("-----END CERTIFICATE-----", "").replace("\n", "").replace("\r", "");
			PackedByteArray cert_der = Marshalls::get_singleton()->base64_to_raw(cert_clean);
			PackedByteArray cert_sig = _extract_signature_from_der(cert_der);

			// 2. 生成客户端密钥（16 字节）
			client_pairing_secret = _generate_random_bytes(16);

			// 3. 构建负载：服务器挑战   证书签名   客户端密钥
			PackedByteArray payload;
			payload.append_array(server_challenge); // 根据 nvhttp.cpp 从 server_secret 修正
			payload.append_array(cert_sig);
			payload.append_array(client_pairing_secret);

			// 4. 哈希 SHA256
			PackedByteArray hash = _sha256(payload);

			// 5. 加密哈希
			PackedByteArray hash_enc = _encrypt_aes_ecb(hash, pair_aes_key);

			String url = base_url + "?" + common_params + "&serverchallengeresp=" + _bytes_to_hex(hash_enc);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(3));
			break;
		}
		case PAIR_STAGE_4_CLIENT_SECRET: {
			// 第4阶段：发送客户端密钥和签名
			// 有效载荷：客户端密钥   签名(客户端密钥)
			// 注意：根据 client.c 和 nvhttp.cpp，这一步不是加密的

			// 1. 使用客户端私钥对客户端密钥进行签名
			PackedByteArray signature = _sign_data(client_pairing_secret);

			// 2. 连接
			PackedByteArray payload = client_pairing_secret;
			payload.append_array(signature);

			// 3. 以 HEX（纯文本）发送
			String url = base_url + "?" + common_params + "&clientpairingsecret=" + _bytes_to_hex(payload);
			requester->request(url, "GET", PackedByteArray(), Dictionary(), ssl_opts, callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(4));
			break;
		}
		case PAIR_STAGE_5_HTTPS_CHALLENGE: {
			// Stage 5: Completed via HTTPS
			String https_url = "https://" + pair_ip + ":" + String::num_int64(pair_https_port) + "/pair";
			String url = https_url + "?" + common_params + "&phrase=pairchallenge";

			// 现在必须使用 SSL 选项
			requester->request(url, "GET", PackedByteArray(), Dictionary(), _get_ssl_options(), callable_mp(this, &ComputerManager::_on_pair_request_completed).bind(5));
			break;
		}
	}
}

void ComputerManager::_on_pair_request_completed(int code, PackedByteArray body, Dictionary headers, String error, int step) {
	is_requesting = false;

	if (code != 200) {
		//Special handling: If waiting for a PIN, the server might not immediately return 200? In practice, GFE/Sunshine usually either blocks or returns 200 with paired=0. If it's a real network error:
		if (code == 0 || code >= 400) {
			pair_state = PAIR_ERROR;
			emit_signal("pair_completed", false, "Network Error: " + String::num_int64(code) + " " + error);
			return;
		}
	}

	String xml = body.get_string_from_utf8();
	bool is_paired = _extract_xml_value(xml, "paired") == "1";

	// Check failed
	if (!is_paired && step != 1) {
		// Except for Stage 1 (waiting for PIN), paired=0 usually indicates failure
		pair_state = PAIR_ERROR;
		String msg = _extract_xml_value(xml, "status_message");
		if (msg.is_empty())
			msg = "Pairing failed at step " + String::num_int64(step);
		emit_signal("pair_completed", false, msg);
		return;
	}

	switch (step) {
		case 1: { // Obtain Certificate
			if (!is_paired) {
				//Waiting for PIN input… If polling, the server may return paired=0, but it usually blocks. If we get paired=0, we may need to retry or fail. Currently, assume that if code=200 and plaincert is missing, it indicates failure or the user has canceled.
				String status_msg = _extract_xml_value(xml, "status_message");
				if (!status_msg.is_empty()) {
					pair_state = PAIR_ERROR;
					emit_signal("pair_completed", false, status_msg);
					return;
				}
				// Retry? Or just fail immediately. The README says it will block.
				// If the code reaches this point, the request has already been completed.
				// Assume failure if there is no certificate.
			}

			String plaincert = _extract_xml_value(xml, "plaincert");
			if (plaincert.is_empty()) {
				pair_state = PAIR_ERROR;
				emit_signal("pair_completed", false, "No server certificate received.");
				return;
			}

			// Store server certificate (from hexadecimal to PEM)
			// If we want to save it as PEM, it needs to be properly formatted, but for internal logic, we might just keep it in hexadecimal or raw format.
			// However, for later verify_signature (if we implement it), we need this key.
			// For now, just store it in raw format.
			server_cert_pem = _hex_to_bytes(plaincert).get_string_from_ascii(); // 假设十六进制解码为 PEM 字符串

			pair_state = PAIR_STAGE_2_CLIENT_CHALLENGE;
			_step_pair();
			break;
		}
		case 2: { // Server response received
			String resp_hex = _extract_xml_value(xml, "challengeresponse");
			if (resp_hex.is_empty()) {
				emit_signal("pair_completed", false, "Empty challenge response");
				return;
			}

			PackedByteArray resp_enc = _hex_to_bytes(resp_hex);
			PackedByteArray resp_dec = _decrypt_aes_ecb(resp_enc, pair_aes_key);

			// 解密数据结构：[SHA256 哈希（32 字节）]   [服务器挑战（16 字节）]
			// 总大小应为 48 字节（SHA1 为 36 字节，但我们使用 SHA256）

			if (resp_dec.size() < 48) {
				emit_signal("pair_completed", false, "Invalid server response size");
				return;
			}

			// 提取 ServerChallenge（最后 16 个字节）
			// client.c：memcpy(challenge_response, challenge_response_data + hash_length, 16);
			server_challenge = resp_dec.slice(32, 48);

			pair_state = PAIR_STAGE_3_SERVER_RESPONSE;
			_step_pair();
			break;
		}
		case 3: { // Pairing key obtained
			String pairing_secret_hex = _extract_xml_value(xml, "pairingsecret");

			// pairingsecret = ServerSecret   Signature(ServerSecret)
			// 如果需要，我们可以提取 ServerSecret，但该协议主要用它来进行中间人攻击验证。
			PackedByteArray pairing_secret = _hex_to_bytes(pairing_secret_hex);
			if (pairing_secret.size() >= 16) {
				server_secret = pairing_secret.slice(0, 16);
			}

			pair_state = PAIR_STAGE_4_CLIENT_SECRET;
			_step_pair();
			break;
		}
		case 4: { // The client key has been sent
			pair_state = PAIR_STAGE_5_HTTPS_CHALLENGE;
			_step_pair();
			break;
		}
		case 5: { // HTTPS 挑战
			// 成功！
			Dictionary host_data;
			host_data["hostname"] = pair_ip;
			host_data["localaddress"] = pair_ip;
			host_data["uuid"] = current_uuid;
			host_data["srvcert"] = server_cert_pem;
			host_data["https_port"] = pair_https_port;
			config_manager->add_host(host_data);

			pair_state = PAIR_FINISHED;
			emit_signal("pair_completed", true, "Pairing successful");
			break;
		}
	}
}

void ComputerManager::cancel_pair() {
	if (pair_state != PAIR_IDLE && pair_state != PAIR_FINISHED && pair_state != PAIR_ERROR) {
		String url = "http://" + pair_ip + ":" + String::num_int64(pair_port) + "/unpair?uniqueid=" + unique_id + "&uuid=" + current_uuid;
		requester->request(url, "GET", PackedByteArray(), Dictionary(), Dictionary(), Callable());
	}
	_reset_pairing();
}

void ComputerManager::unpair(int host_id) {
	if (config_manager) {
		config_manager->remove_host(host_id);
	}
}

void ComputerManager::_reset_pairing() {
	pair_state = PAIR_IDLE;
	is_requesting = false;
	server_cert_pem = "";
	server_secret.clear();
	server_challenge.clear();
	client_secret_random.clear();
	client_pairing_secret.clear();
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
	// 将 PIN 转换为 ASCII 字节（例如 "1234" -> 0x31 0x32 0x33 0x34）
	combined.append_array(pin.to_ascii_buffer());
	// Return the first 16 bytes of SHA256
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

	// Godot Crypto::sign 当使用 HASH_SHA256 时，需要传入 32 字节的哈希摘要(Digest)
	PackedByteArray hash = _sha256(data);
	return c->sign(HashingContext::HASH_SHA256, hash, key);
}

PackedByteArray ComputerManager::_extract_signature_from_der(const PackedByteArray &der) {
	// A minimal ASN.1 parser used to find the last BIT STRING in a sequence
	// X.509 structure: SEQUENCE { ... }
	// Inside: TBSCertificate, AlgorithmIdentifier, BIT STRING (signature)

	int len = der.size();
	const uint8_t *data = der.ptr();
	int pos = 0;

	// 1. Check external sequence
	if (pos >= len || data[pos++] != 0x30)
		return PackedByteArray(); // Not a sequence

	// Skip length byte
	if (pos >= len)
		return PackedByteArray();
	if (data[pos] & 0x80) {
		int len_bytes = data[pos] & 0x7F;
		pos += 1 + len_bytes;
	} else {
		pos++;
	}

	//We expect 3 child elements. The signature is the third one (bit string, tag 0x03). We iterate through the child elements.
	int child_count = 0;
	while (pos < len) {
		uint8_t tag = data[pos];

		// If we find the third element and it is a BIT STRING (0x03)
		if (child_count == 2) {
			if (tag == 0x03) {
				pos++; // Skip tags
				// Parse length
				int val_len = 0;
				if (data[pos] & 0x80) {
					int len_bytes = data[pos] & 0x7F;
					pos++;
					for (int i = 0; i < len_bytes; i++) {
						val_len = (val_len << 8) | data[pos++];
					}
				} else {
					val_len = data[pos++];
				}

				// A BIT STRING has 1 byte at the beginning to count the unused bits.
				if (val_len > 1) {
					// Return bytes, skipping bytes with unused bits
					return der.slice(pos + 1, pos + val_len);
				}
				return PackedByteArray();
			}
			return PackedByteArray(); // Isn't the third element a bit string?
		}

		// Skip current element
		pos++; // Skip tags
		// Parse the length to skip
		int val_len = 0;
		if (pos < len) {
			if (data[pos] & 0x80) {
				int len_bytes = data[pos] & 0x7F;
				pos++;
				for (int i = 0; i < len_bytes; i++) {
					if (pos >= len)
						return PackedByteArray();
					val_len = (val_len << 8) | data[pos++];
				}
			} else {
				val_len = data[pos++];
			}
		}
		pos += val_len;
		child_count++;
	}

	return PackedByteArray();
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
	return _bytes_to_hex(_generate_random_bytes(16)); // A standard UUID is 16 bytes
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

	ADD_SIGNAL(MethodInfo("pair_completed", PropertyInfo(Variant::BOOL, "success"), PropertyInfo(Variant::STRING, "message")));
}
